// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, string_concat_accepted).
//
// String compound concatenation (§4.6) is untouched: the target's kind is
// `string`, which this check explicitly excludes (concatenation has its own
// typing rule, not §6.2's numeric promotion table).
//
// #2157 (§4.6/§6.6, spec v0.226): the retired `string + T` conversion rule
// was removed from this cell — `s += 5` is now a §4.6 compile error (matching
// `s + 5`), covered by compound_string_target_2157__string_plus_equals_int_rejected.bzl.
// Only the `string += string` concat form remains here.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var s: string := "a"; s += "b"; return s.Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
