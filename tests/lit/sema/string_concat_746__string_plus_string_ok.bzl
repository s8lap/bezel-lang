// Ported from tests/sema/string_concat_746_test.cpp
//   (BEZEL_TEST string_concat_746, string_plus_string_ok).
//
// Source built by hand from the file's `mainOf(expr)` C++ helper (a
// `const char*`-parameter string builder the extractor's automatic
// string-helper resolver does not match) — expanded here to
// `mainOf("\"a\" + \"b\"")`.
//
// §4.6: `string + string` is valid concatenation — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static method Main(): int   var s := "a" + "b"; return s.Length; end Main; end F;

// NODIAG-NOT: error
// NODIAG-NOT: §
