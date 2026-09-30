// RUN: %bezelc -c -o %t.o %s
//
// §6.6, v0.313 (#5211) controls: `int`/`long` and constant right operands stay
// admitted for a `char` compound target; only a non-constant uint/ulong is not.

module M; class C public static method Main(): int var c: char := char(100); var i: int := 5; var l: long := 7; c += i; c -= l; c += 3; c++; c--; return int(c); end Main; end C;
