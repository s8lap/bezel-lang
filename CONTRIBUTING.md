# Contributing to Bezel

Thank you for helping. Bezel is experimental, and the most useful contributions right now are **bug reports**, **minimal reproductions** and **fixes that bring the compiler in line with the specification**.

## The one rule: the specification decides

Bezel is defined by its [specification](https://s8lap.github.io/bezel-lang/spec/) and [standard library](https://s8lap.github.io/bezel-lang/stdlib/) documents. When the compiler and the specification disagree, the compiler is wrong, even if the compiler's behaviour seems better. When the specification itself looks wrong, unclear or silent, open an issue labelled `spec-question` instead of changing the code to match your reading. Changes to the language go through the [spec-change process](docs/proposals/spec-change-process.md), usually with a proposal in [`docs/proposals/`](docs/proposals/README.md).

For anything larger than a small fix, open or comment on an issue first so the work is not duplicated.

## Building

Bezel builds on Linux with clang and LLVM 21 (C++20). It is developed on Fedora; other distributions may need small changes to the package names.

```sh
sudo dnf install cmake clang llvm llvm-devel
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)                       # the compiler: build/bezelc
cmake --build build -j$(nproc) --target bezel_tests  # the test binaries
```

Use clang, not GCC; the build and CI assume it.

## Running the tests

The behaviour tests are several gtest binaries, one per area of the compiler, listed in `build/tests/test_binaries.txt`.

```sh
BEZEL_TEST_FILTER=nullable build/tests/bezel_tests_sema   # tests whose name contains "nullable"
tests/run_parallel.sh 16                                  # every behaviour test, sharded
cmake --build build --target check-lit                    # the diagnostic tests (needs lit and FileCheck)
scripts/check_docs.sh                                     # the documentation checks
```

The binaries ignore `--gtest_filter`; use `BEZEL_TEST_FILTER`. After changing anything under `src/`, rebuild the test binary you run, not just `bezelc`: the tests compile programs with the compiler linked into the test binary itself.

The full suite takes a while and a lot of memory. For a change of your own, running the tests near it is enough; the maintainers run the full suite, with and without AddressSanitizer, before anything lands on `main`.

## Making a fix

1. **Reproduce it** with the smallest program you can, and find the section of the specification that says what should happen. Cite it with its revision, for example "§28.2, v0.340".
2. **Write the regression test first.** It uses the issue's program, fails without your fix and passes with it. Check both directions; do not assume. [`tests/README.md`](tests/README.md) explains the helpers: new behaviour tests use `runAot`, which compiles and runs the program for real, and compile errors are checked in the lit suite under `tests/lit/`.
3. **Fix the compiler**, matching the style of the code around it. Comments cite the specification section that non-obvious code follows (`// per §17.4`).
4. **Never weaken an existing test to make it pass.** If your change legitimately flips a behaviour, update every test that pinned the old one and say so in the commit message. If you think a test is wrong, say why in the pull request instead of editing it quietly.

Keep a pull request to one issue, or a few closely related ones. No new dependencies without discussing them first.

## Commit messages

Imperative mood and a short subject line; the body explains why, names the issue (`fix #1234`) and cites the specification section and revision. If the change makes a test flip, list the test and its new expectation.

## Documentation

Typos and broken links in the README or the Tour are welcome as plain pull requests. The specification, the standard library, the rationale, the changelog, the guides and the writing reference change only through the spec-change process, even for wording; open an issue describing the problem.

## Licence

Bezel is licensed under [Apache 2.0](LICENSE). By contributing, you agree that your contribution is licensed under the same terms.
