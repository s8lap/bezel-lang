// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, full_explicit_list_accepted).
//
// Positive control: a FULL explicit type-argument list (2 of 2) is
// accepted — the all-or-nothing check must not over-reject the valid form.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Utils public static method Pick<TFirst, TSecond>(a: TFirst, b: TSecond): TFirst return a; end Pick; end Utils; class C public static method Main(): int var x := Utils.Pick<int, string>(1, "s"); return x; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
