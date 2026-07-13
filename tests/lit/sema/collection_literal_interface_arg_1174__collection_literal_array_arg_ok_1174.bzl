// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST collection_literal_interface_arg_1174, collection_literal_array_arg_ok_1174).
//
// §15.4 (#1174) control: the same literal passed to an `Array<int>` parameter
// (a concrete, admissible target) is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Sum(xs: Array<int>): int var n := 0; for var x in xs do n := n + x; end; return n; end Sum; public static method Main(): int return Sum([1, 2, 3]); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
