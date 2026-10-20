# Third-party notices

TtsEngine-C (MIT) builds on the following third-party components. Each entry
lists the upstream project, the version used, the license, and how the code
reaches this repository.

## 1. qwentts.cpp — inference engine

- Project: <https://github.com/ServeurpersoCom/qwentts.cpp>
- Version: commit `5916bf7` (2026-10-16, "ggml: fork rebase")
- License: MIT — Copyright (c) 2023-2026 The omnivoice.cpp authors
- Integration: cloned by `tools/fetch_upstream.ps1` into
  `third_party/qwentts.cpp` (git-ignored). TtsEngine-C calls its C ABI
  (`qwen.h`) unmodified; no upstream source is copied into this repository.
- Local modifications: none. All TtsEngine-C changes live in `src/`.

If you redistribute binaries built from this project, include this notice and
the upstream MIT license text (reproduced in §5).

## 2. ggml — tensor library and GPU backends

- Project: <https://github.com/ggml-org/ggml>
- Version: the submodule revision pinned by qwentts.cpp @ `5916bf7`
- License: MIT — Copyright (c) 2023-2026 The ggml authors
- Integration: submodule of the upstream tree; provides `ggml.dll`,
  `ggml-cpu.dll`, `ggml-cuda.dll` at build time.

## 3. cpp-httplib — HTTP server

- Project: <https://github.com/yhirose/cpp-httplib>
- Version: 0.46.1 (vendored inside qwentts.cpp)
- License: MIT — Copyright (c) 2017 yhirose
- Integration: header/implementation under `third_party/qwentts.cpp/vendor/cpp-httplib`.

## 4. yyjson — JSON parser

- Project: <https://github.com/ibireme/yyjson>
- Version: as vendored inside qwentts.cpp (single `yyjson.c`/`yyjson.h` pair)
- License: MIT — Copyright (c) 2020 YaoYuan
- Integration: `third_party/qwentts.cpp/vendor/yyjson`.

## 5. MIT license text (applies to components 1-4)

```
MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Copyright lines for the components above:

- qwentts.cpp: `Copyright (c) 2023-2026 The omnivoice.cpp authors`
- ggml: `Copyright (c) 2023-2026 The ggml authors`
- cpp-httplib: `Copyright (c) 2017 yhirose`
- yyjson: `Copyright (c) 2020 YaoYuan`

## 6. Models — not distributed

- Qwen3-TTS-12Hz-1.7B-Base / -VoiceDesign: Qwen team, Apache-2.0
  (<https://huggingface.co/Qwen/Qwen3-TTS-12Hz-1.7B-Base> and the VoiceDesign
  sibling).
- Qwen3-TTS-Tokenizer-12Hz: Qwen team, Apache-2.0.

No model weights, no converted GGUF files and no reference audio are included
in this repository. The 44 built-in "voice presets" are text descriptions
only.
