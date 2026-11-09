# Third-party notices

AScript itself is licensed under MIT (see [LICENSE](LICENSE)).

## Linked into the release binaries

- **AlxLib** (MIT) — the `alxscpt` engine and base libraries. Vendored headers and docs live under `lib/alxlib/`; its own manifest and full texts are `lib/alxlib/THIRD-PARTY.md` and `lib/alxlib/licenses/`.
- **LZ4** (BSD-2-Clause) — linked through AlxLib; full text in [`licenses/lz4.txt`](licenses/lz4.txt).
- **zstd** (BSD-3-Clause) — linked through AlxLib; full text in [`licenses/zstd.txt`](licenses/zstd.txt).
- **zlib** (zlib License) — linked through AlxLib; full text in [`licenses/zlib.txt`](licenses/zlib.txt).

## GUI

- **Qt 6** (LGPL-3.0-only) — `axc-gui` links Qt 6 dynamically. Qt is not distributed with this repository or its release packages. Full texts: [`licenses/lgpl-3.0.txt`](licenses/lgpl-3.0.txt), [`licenses/gpl-3.0.txt`](licenses/gpl-3.0.txt).

## Optional link modules

- **OpenCV 4.14.0** (Apache-2.0) — the `cv` link embeds OpenCV statically when built. Its bundled third-party notices are installed with the OpenCV tree (`share/licenses/opencv4/` on Linux, `etc/licenses/` on Windows) and must ship with any binary redistribution of the `cv` module. Apache-2.0 text: [`licenses/apache-2.0.txt`](licenses/apache-2.0.txt). Build notes: [`lib/opencv/README.md`](lib/opencv/README.md).
- **Ollama** — `links/ollama` is an unofficial HTTP client for the Ollama API. It contains no Ollama code and is not affiliated with the Ollama project.

## Fonts

- **Fira Code 6.002** (`res/gui/FiraCode.ttf`) — SIL Open Font License 1.1; full text in [`licenses/FiraCode.txt`](licenses/FiraCode.txt).

## Icons

- The settings/gear glyph in `res/gui/icons/gear.svg` is derived from **Feather Icons** (MIT, Copyright (c) 2013-2023 Cole Bemis); full text in [`licenses/feather.txt`](licenses/feather.txt). The remaining icons are project-local SVGs.

## Notes

- AlxLib's own third-party components (PCRE2, SQLite3, libjpeg-turbo, OpenSSL and others) are listed in `lib/alxlib/THIRD-PARTY.md`; the release CLI does not link all of them.
