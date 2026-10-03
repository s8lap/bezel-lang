// RUN: %bezelc -c -o %t.o %s
//
// §6.6, v0.313 (#5287): a `char` compound target's right operand is "typed as
// in `int op e` under §6.2 — `sbyte`, `byte`, `short`, `ushort`, `int` or
// `long`, or an integer constant". `byte` is in that list, so `c += b` is the
// admitted advance `c := char(int(c) + int(b))`.
//
// Before the fix this was rejected with the §6.2/§6.6 pair diagnostic
// ("has no implicit conversion between operand types `char` and `byte`"):
// `byte` never widens to `char` under §5.1, so the generic pair test fired,
// while the pair §6.6 actually types (`int op byte`) is §6.2-admitted.
// One row per admitted type; the runtime values are pinned in
// tests/codegen/char_compound_narrow_rhs_5287_test.cpp.

module M; class C public static method Main(): int var c: char := char(50); var b: byte := byte(2); c += b; return int(c); end Main; end C;
