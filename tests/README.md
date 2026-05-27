# Tests

Tests follow the per-milestone workflow described in `CLAUDE.md`:

1. A milestone's **first** PR adds spec-derived tests — every example from the
   spec section the milestone covers, with the spec-stated input and expected
   output. These tests *fail* until the implementation lands; that CI failure is
   expected.
2. The tests are reviewed as a translation of the spec into executable form.
3. Implementation PRs follow until the tests pass, without modifying the tests
   except to fix legitimate test bugs.

Each test names the spec section and example it derives from, e.g.
`BEZEL_TEST(lexer_string_interpolation, spec_1_5_example_3)`.

## Layout

The directories here mirror the compiler phases under `src/` and are where each
phase's spec-derived tests go:

- `lexer/` — §1, §18, §34, Appendix B
- `parser/` — Appendix A, §17, §19–§21
- `sema/` — §3–§14
- `ownership/` — §28, Appendix C

They are empty placeholders today. When you add a test file, also list it in
`BEZEL_TEST_SOURCES` in `tests/CMakeLists.txt` (sources are listed explicitly,
not globbed, so each addition is a reviewable diff).

## Harness

`test_harness.hpp` is a minimal, zero-dependency harness (register with
`BEZEL_TEST`, assert with `BEZEL_EXPECT_TRUE` / `BEZEL_EXPECT_FALSE` /
`BEZEL_EXPECT_EQ`). Adopting a third-party framework (GoogleTest, Catch2, …) is
a deferred project-level decision; the harness surface is deliberately small to
keep that migration cheap. `sanity_test.cpp` only verifies the harness itself.
