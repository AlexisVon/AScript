# Contributing

Thanks for wanting to help. Two rules keep this project what it is: the CLI gate stays green, and the language reference stays in sync with the code.

## Reporting bugs

Open an issue with a minimal reproducer -- a script fragment or an `axc-cli -e '...'` line beats a description.

## Building

Linux:

```bash
# AlxLib shared package -> lib/alxlib/ (see the README)
cmake -B build -DASCRIPT_OPT_LEVEL=2
cmake --build build -j7
```

Windows: `msvc/AScript.slnx` (Visual Studio 2022, x64); see the README's Windows/MSVC section.

## The gate must be green

```bash
(cd test/cli && make && bash test.sh)
bash test/runner/run.sh   # needs -DBUILD_REPLAY=ON
bash test/gui/run.sh      # needs -DBUILD_GUI_TESTS=ON
```

`test/cli/cip`, `test/cli/net`, `test/cli/libpaths` and `test/cli/cv` (OpenCV) are optional; run them when touching those areas.

## Style

- C++17, GCC/Clang (MSVC on Windows).
- Comments in English, only where the code cannot say it itself -- one crucial line.
- `clang-format -i` with the shipped `.clang-format`.
- Header declarations must match definition order in the `.cpp` of the same file pair.
- Resource docs: `res/core/*.json` is the function contract; regenerate `src/core/res.cpp` with `python3 tools/gen_res.py res/ core src/core/res.cpp` after editing.

## Sign your commits (DCO)

This project uses the Developer Certificate of Origin (see `DCO`) instead of a CLA. Sign every commit off:

```bash
git commit -s
```

Signing off certifies the statements in `DCO` -- briefly: the contribution is yours to submit, under this project's license (MIT, inbound = outbound).
