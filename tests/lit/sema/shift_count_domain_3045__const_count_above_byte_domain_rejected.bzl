// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §6.5 (v0.242, proposal 0079 / #3045) — the promotion trap. The domain is the
// LEFT operand's own width: "The right operand supplies only the shift count: it
// does not participate in the mixed-type promotion of §6.2, so a narrower left
// operand is *not* widened to match a wider count (`byte << int` yields `byte`,
// not `int`)." So a `byte` left operand's domain is `0..7` — a count of 8 is out
// of domain even though 8 is an unremarkable `int` count and the count's own
// type here IS `int`. The diagnostic's mask spelling follows the width too:
// `& 7`, not `& 31`.
// Before v0.242 this compiled and silently yielded `1 << (8 mod 8)` == 1.

module M; class C public static method Main(): int var v: byte := 1; var r: byte := v << 8; return int(r); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: shift count 8 is outside the domain `0..7` of the left operand's type `byte`
// CHECK-SAME: (§6.5/§17.14)
// CHECK-SAME: write `x << (k & 7)` if the modular count is intended
