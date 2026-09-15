// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST collection_literal_interface_arg_1174, string_trim_concrete_array_arg_ok_1174).
//
// §15.4 (#1174) control: `string.Trim` given a CONCRETE `Array<char>` value
// (not a literal) stays valid.
//
// NOTE: literal is now Owned per fix #4338 fix-forward; changed `var` to `const`
// to avoid the auto-drop warning (value-typed Array<char> has no disposal obligation).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): int const cs: Array<char> := ['x']; var s := "xxhixx".Trim(cs); return s.Length; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
