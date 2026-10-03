// RUN: %bezelc -c -o %t.o %s
//
// §6.6, v0.313 (#5287) — see char_compound_narrow_rhs_accepted_5287__byte_plus.bzl
// for the rule. `sbyte` is the first entry of §6.6's `int op e` list.

module M; class C public static method Main(): int var c: char := char(50); var b: sbyte := sbyte(-1); c += b; return int(c); end Main; end C;
