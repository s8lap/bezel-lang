// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §35.10 (v0.200, #2044): "`default(C)` is a compile error for every
// choice type — a sum has no principled zero: electing the first-declared
// case would make program meaning depend on declaration order, and
// restricting eligibility to first-case-payload-free would make
// eligibility order-dependent, which is worse."
//
// The nullable form `default(C?)` is the valid null value (§35.9) and stays
// accepted — not exercised by this negative test.

module M;

choice Opt Some(value: int), None; end Opt;

class C public static method Main(): int var x: Opt := default(Opt); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `default(T)` is a compile error for a choice type (no case is distinguished; §35.10) — it has no well-defined zero value; write `default(T?)` for the nullable counterpart where applicable
// CHECK-SAME: (§17.11)
