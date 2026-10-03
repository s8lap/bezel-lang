// RUN: %bezelc -c -o %t.o %s
//
// §6.6, v0.313 (#5287) — see char_compound_narrow_rhs_accepted_5287__byte_plus.bzl
// for the rule. `short` with `-=`: §6.6 admits both `+=` and `-=` on a `char`
// target, so the fix is not `+=`-only.

module M; class C public static method Main(): int var c: char := char(50); var b: short := short(7); c -= b; return int(c); end Main; end C;
