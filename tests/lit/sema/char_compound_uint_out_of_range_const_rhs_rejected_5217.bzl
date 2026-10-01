// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §6.6, v0.313 / §5.1 / §17.14 (#5217): a `char` compound target's right
// operand is typed as in `int op e` under §6.2; §5.1's constant narrowing
// admits a compile-time constant (§17.14) of any integer type whose value
// fits the target's range. `char`'s range per §5.2 is [0, 0x10FFFF]
// excluding the surrogate block [0xD800, 0xDFFF]. A compile-time uint
// constant whose value exceeds `char`'s range is a §5.1 compile error at
// the conversion site, never a silent wrap. Before the fix the #5211
// predicate `isLitDerived` admitted the literal `4294967295U` (syntactic
// literal) and codegen silently wrapped it to char 99 (no
// `OverflowException`); the issue's repro was the silent-wrong-value
// admission. Post-fix the constant-narrowing range check fires.

module M; class C public static method Main(): int var c: char := char(100); c += 4294967295U; return int(c); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` on a `char` target: the constant `4294967295` does not fit `char`'s code-point range
// CHECK-SAME: (§5.2: [0, 0x10FFFF] excluding the surrogate block [0xD800, 0xDFFF])
// CHECK-SAME: constant narrowing is rejected for an out-of-range value (§5.1, v0.313)