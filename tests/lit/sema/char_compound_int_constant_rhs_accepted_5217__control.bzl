// RUN: %bezelc -c -o %t.o %s
//
// §6.6, v0.313 (#5217) control row — pre-fix already passes. `c += 5;` on
// a `char` target with an `int` constant RHS (5 is in `char`'s
// [0, 0x10FFFF] range) is admitted both by the #5211 rule (an
// `int`-typed constant narrowing to `char`) and by the #5217 rule (a §17.14
// constant in range). Pin the existing accept behavior so #5217's
// constant-narrowing rewrite cannot silently regress the literal case.

module M; class C public static method Main(): int var c: char := char(100); c += 5; c -= 2; c++; c--; return int(c); end Main; end C;