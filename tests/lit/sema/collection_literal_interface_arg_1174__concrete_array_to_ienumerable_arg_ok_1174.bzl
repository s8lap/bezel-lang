// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST collection_literal_interface_arg_1174, concrete_array_to_ienumerable_arg_ok_1174).
//
// §15.4 (#1174) control: a CONCRETE `Array<int>` passed to an
// `IEnumerable<int>` parameter is accepted — only the literal is rejected.
//
// NOTE: literal is now Owned per fix #4338 fix-forward; changed `var` to `const`
// to avoid the auto-drop warning (value-typed Array<int> has no disposal obligation).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Sum(xs: IEnumerable<int>): int var n := 0; for var x in xs do n := n + x; end; return n; end Sum; public static method Main(): int const a: Array<int> := [1, 2, 3]; return Sum(a); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
