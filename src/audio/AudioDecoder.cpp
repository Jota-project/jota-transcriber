#include "AudioDecoder.h"
#include <stdexcept>
#include <string>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {

struct ReadState {
    const uint8_t* base;
    size_t         size;
    size_t         pos;
};

static int readPacket(void* opaque, uint8_t* buf, int buf_size) {
    auto* s = static_cast<ReadState*>(opaque);
    int n = static_cast<int>(std::min(static_cast<size_t>(buf_size), s->size - s->pos));
    if (n <= 0) return AVERROR_EOF;
    memcpy(buf, s->base + s->pos, n);
    s->pos += n;
    return n;
}

static int64_t seekPacket(void* opaque, int64_t offset, int whence) {
    auto* s = static_cast<ReadState*>(opaque);
    int direction = whence & ~AVSEEK_FORCE; // strip the flag before switching
    if (direction == AVSEEK_SIZE)
        return static_cast<int64_t>(s->size);
    int64_t new_pos;
    if      (direction == SEEK_SET) new_pos = offset;
    else if (direction == SEEK_CUR) new_pos = static_cast<int64_t>(s->pos) + offset;
    else if (direction == SEEK_END) new_pos = static_cast<int64_t>(s->size) + offset;
    else return -1;
    if (new_pos < 0 || new_pos > static_cast<int64_t>(s->size)) return -1;
    s->pos = static_cast<size_t>(new_pos);
    return new_pos;
}

} // namespace

std::vector<float> AudioDecoder::decode(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        throw std::runtime_error("AudioDecoder: empty input");
    }

    // ── AVIO in-memory context ────────────────────────────────────────────────
    const int AVIO_BUF = 4096;
    uint8_t* avio_buf = static_cast<uint8_t*>(av_malloc(AVIO_BUF));
    if (!avio_buf) throw std::runtime_error("AudioDecoder: av_malloc failed");

    ReadState state{data.data(), data.size(), 0};
    AVIOContext* avio_ctx = avio_alloc_context(
        avio_buf, AVIO_BUF, 0, &state, readPacket, nullptr, seekPacket);
    if (!avio_ctx) {
        av_free(avio_buf);
        throw std::runtime_error("AudioDecoder: avio_alloc_context failed");
    }

    // ── Format / stream detection ─────────────────────────────────────────────
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    fmt_ctx->pb = avio_ctx;

    if (avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr) < 0) {
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        throw std::runtime_error("AudioDecoder: failed to open input — unrecognised format");
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        throw std::runtime_error("AudioDecoder: failed to find stream info");
    }

    int audio_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = static_cast<int>(i);
            break;
        }
    }
    if (audio_idx < 0) {
        avformat_close_input(&fmt_ctx);
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        throw std::runtime_error("AudioDecoder: no audio stream found");
    }

    // ── Decoder ───────────────────────────────────────────────────────────────
    AVCodecParameters* cpar = fmt_ctx->streams[audio_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(cpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt_ctx);
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        throw std::runtime_error("AudioDecoder: no decoder found for codec id " +
                                 std::to_string(cpar->codec_id));
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, cpar);
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        throw std::runtime_error("AudioDecoder: failed to open codec");
    }

    // ── Resampler → 16 kHz mono s16 ──────────────────────────────────────────
    SwrContext* swr = swr_alloc();
#if LIBAVCODEC_VERSION_MAJOR > 60
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO;
    av_opt_set_chlayout(swr, "in_chlayout",  &codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &out_layout, 0);
#else
    int64_t in_layout = codec_ctx->channel_layout
        ? codec_ctx->channel_layout
        : av_get_default_channel_layout(codec_ctx->channels);
    av_opt_set_int(swr, "in_channel_layout",  in_layout,          0);
    av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_MONO,  0);
    av_opt_set_int(swr, "in_channel_count",   codec_ctx->channels,0);
    av_opt_set_int(swr, "out_channel_count",  1,                  0);
#endif
    av_opt_set_int(swr,        "in_sample_rate",  codec_ctx->sample_rate, 0);
    av_opt_set_int(swr,        "out_sample_rate", 16000,                  0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt",   codec_ctx->sample_fmt,  0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt",  AV_SAMPLE_FMT_S16,      0);
    swr_init(swr);

    // ── Decode + resample ─────────────────────────────────────────────────────
    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    std::vector<int16_t> s16;

    auto drain_swr = [&](AVFrame* f) {
        int64_t delay = swr_get_delay(swr, codec_ctx->sample_rate);
        int out_n = static_cast<int>(av_rescale_rnd(
            delay + (f ? f->nb_samples : 0),
            16000, codec_ctx->sample_rate, AV_ROUND_UP));
        if (out_n <= 0) return;
        std::vector<int16_t> buf(out_n);
        uint8_t* out_ptr = reinterpret_cast<uint8_t*>(buf.data());
        int converted = swr_convert(swr, &out_ptr, out_n,
            f ? const_cast<const uint8_t**>(f->data) : nullptr,
            f ? f->nb_samples : 0);
        if (converted > 0)
            s16.insert(s16.end(), buf.begin(), buf.begin() + converted);
    };

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == audio_idx) {
            avcodec_send_packet(codec_ctx, pkt);
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                drain_swr(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }
    drain_swr(nullptr); // flush resampler

    // ── Cleanup ───────────────────────────────────────────────────────────────
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);

    if (s16.empty())
        throw std::runtime_error("AudioDecoder: decoded audio is empty");

    // ── s16 → float32 ────────────────────────────────────────────────────────
    std::vector<float> pcm(s16.size());
    for (size_t i = 0; i < s16.size(); ++i)
        pcm[i] = static_cast<float>(s16[i]) / 32768.0f;
    return pcm;
}