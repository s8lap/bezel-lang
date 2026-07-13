// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, string_concat_accepted).
//
// String compound concatenation (§4.6) is untouched: the target's kind is
// `string`, which this check explicitly excludes (concatenation has its own
// typing rule, not §6.2's numeric promotion table).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var s: string := "a"; s += "b"; s += 5; return s.Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
