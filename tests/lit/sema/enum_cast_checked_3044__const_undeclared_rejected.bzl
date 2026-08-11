// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§5.4, v0.242): `E(n)` is CHECKED AGAINST THE DECLARED CONSTANTS —
// "it succeeds exactly when `n` equals the underlying value of some declared
// constant of `E`". When `n` is a §17.14 compile-time constant equal to no
// declared constant, "the cast is a compile error at the cast site".
//
// `Priority` declares 0, 1, 2 (§10.1 auto-increment from zero), so the IN-RANGE
// constant 7 is a declared-value miss — accepted through v0.241 (it minted a
// "raw" enum value equal to no declared constant), rejected from v0.242.
//
// WRONG-without: compiles clean, `p` holds a value no `case` arm can name.
// CORRECT-with: the §5.4 constant arm rejects at the cast site.

module M; enum Priority Low, Medium, High; end Priority; class C public static method Main(): int var p: Priority := Priority(7); return int(p); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum cast `Priority(7)` — 7 is not the underlying value of any declared constant of `Priority`
// CHECK-SAME: (§5.4)
