---
name: whisper-realtime
description: >
  Expert skill for real-time audio transcription using whisper.cpp in C++ projects.
  ALWAYS use this skill when the user is working with: whisper.cpp (submodule or dependency),
  StreamingWhisperEngine, transcribeSlidingWindow(), ModelCache, InferenceLimiter,
  real-time audio via WebSocket, sliding window buffering, lag accumulation in transcription,
  VAD tuning, whisper_full_with_state(), whisper_full_params tuning, CMake integration of
  whisper.cpp as a git submodule, threading models for non-blocking inference, GGML/CUDA build
  issues, or any C++ server receiving PCM audio over WebSocket and transcribing with Whisper.
  Also trigger for: audio buffer design, commit/partial text patterns, high-pass filtering of
  PCM, beam search vs greedy tradeoffs, or performance problems in whisper inference.
  This skill contains battle-tested patterns specific to the two-tier architecture
  (StreamingSession + StreamingWhisperEngine) with ModelCache singleton.
---

# whisper-realtime Skill

Expert knowledge for the exact architecture of this project.

Read `references/sliding-window-deep-dive.md` for detailed algorithm analysis.
Read `references/cmake-submodule.md` for build troubleshooting.