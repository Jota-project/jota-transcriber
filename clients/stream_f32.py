#!/usr/bin/env python3
"""
Streaming client for race condition / performance testing.

Reads raw float32 PCM (16kHz mono) and streams via WebSocket.
Supports faster-than-realtime sending to stress concurrent buffer access.

Usage:
    python3 clients/stream_f32.py Audio-prueba-jota-transcriber.f32
    python3 clients/stream_f32.py Audio-prueba-jota-transcriber.f32 --speed 5
    python3 clients/stream_f32.py Audio-prueba-jota-transcriber.f32 --speed 0  # as fast as possible
"""

import asyncio
import websockets
import json
import struct
import sys
import argparse
import time
from pathlib import Path


async def stream(f32_path: str, url: str, speed: float, chunk_ms: int, language: str, token: str):
    print(f"Connecting to {url}...")
    async with websockets.connect(url, open_timeout=10) as ws:
        # Config
        cfg = {"type": "config", "language": language}
        if token:
            cfg["token"] = token
        await ws.send(json.dumps(cfg))
        resp = json.loads(await ws.recv())
        if resp.get("type") != "ready":
            print(f"Unexpected: {resp}")
            return
        print(f"Ready: session={resp.get('session_id','?')}")

        # Load PCM
        data = Path(f32_path).read_bytes()
        n_samples = len(data) // 4
        duration_s = n_samples / 16000
        print(f"Audio: {n_samples} samples = {duration_s:.2f}s")

        chunk_samples = int(16000 * chunk_ms / 1000)
        chunk_bytes = chunk_samples * 4  # float32
        sleep_s = (chunk_ms / 1000.0) / speed if speed > 0 else 0

        print(f"Streaming: chunk={chunk_ms}ms, speed={speed}x, sleep={sleep_s*1000:.1f}ms/chunk")

        offset = 0
        n_chunks = 0
        t_start = time.monotonic()

        while offset < len(data):
            chunk = data[offset:offset + chunk_bytes]
            if not chunk:
                break
            await ws.send(chunk)
            offset += chunk_bytes
            n_chunks += 1

            # Drain any incoming messages without blocking
            try:
                while True:
                    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=0.001))
                    _print_msg(msg)
            except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
                pass

            if sleep_s > 0:
                await asyncio.sleep(sleep_s)

        elapsed = time.monotonic() - t_start
        print(f"\nSent {n_chunks} chunks in {elapsed:.2f}s (audio={duration_s:.2f}s, actual speed={duration_s/elapsed:.1f}x)")

        # End signal
        await ws.send(json.dumps({"type": "end"}))
        print("Sent end, waiting for final transcription...")

        try:
            async with asyncio.timeout(30):
                while True:
                    msg = json.loads(await ws.recv())
                    _print_msg(msg)
                    if msg.get("type") == "transcription" and msg.get("is_final"):
                        break
        except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
            print("(connection closed or timeout)")

    print("Done.")


def _print_msg(msg):
    t = msg.get("type")
    if t == "transcription":
        marker = "FINAL" if msg.get("is_final") else "partial"
        print(f"  [{marker}] {msg.get('text','')!r}")
    elif t == "warning":
        print(f"  [WARNING] {msg.get('code')}: {msg.get('message')}")
    elif t == "error":
        print(f"  [ERROR] {msg.get('message')}")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("f32", help="Raw float32 PCM file (16kHz mono)")
    p.add_argument("--url", default="ws://localhost:8003", help="WebSocket URL")
    p.add_argument("--speed", type=float, default=1.0,
                   help="Send speed multiplier (1=realtime, 5=5x faster, 0=max speed)")
    p.add_argument("--chunk-ms", type=int, default=250, help="Chunk size in ms")
    p.add_argument("--language", default="es")
    p.add_argument("--token", default="")
    args = p.parse_args()

    asyncio.run(stream(args.f32, args.url, args.speed, args.chunk_ms, args.language, args.token))
