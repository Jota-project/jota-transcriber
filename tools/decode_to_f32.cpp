#include "audio/AudioDecoder.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: decode_to_f32 <input_audio> <output.f32>\n";
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::cerr << "Cannot open: " << argv[1] << "\n"; return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});

    std::vector<float> pcm;
    try {
        pcm = AudioDecoder::decode(data);
    } catch (const std::exception& e) {
        std::cerr << "Decode error: " << e.what() << "\n";
        return 1;
    }

    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(pcm.data()), pcm.size() * sizeof(float));
    std::cerr << "OK: " << pcm.size() << " samples = "
              << pcm.size() / 16000.0f << "s @ 16kHz\n";
    return 0;
}
