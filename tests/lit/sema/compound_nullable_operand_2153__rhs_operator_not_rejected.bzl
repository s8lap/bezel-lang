// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §25.1 / §25.2, spec v0.220) — the Named-RHS reject must NOT
// fire when the RHS type declares an applicable operator overload: §25.2
// admits "a binary overload may pair the declaring type with any other type
// in either position", and the binary form `n * s` resolves through Scale's
// `operator *(int, Scale): int` and runs (probe: 42). The compound `n *= s`
// is then well-typed per §25.1 (the int result stores back to the int
// target), so sema must stay silent on it.
//
// NOTE: codegen currently fails this program with a malformed-IR abort — the
// compound path dispatches only operators declared on the TARGET's type
// (LlvmBackend_Stmts.cpp #610 block), not the RHS's (tracked in the #2153
// follow-up issue). This test pins the SEMA admission only: the compile may
// fail, but never with the §6.6 operand-check diagnostics. When the codegen
// issue is fixed, convert this into a value-verifying gtest.

module M;
record Scale(F: int)
  public static method operator *(n: int, s: Scale): int => n * s.F;
end Scale;
class C
  public static method Main(): int
    var n: int := 6;
    var s: Scale := new Scale(7);
    n *= s;
    return n;
  end Main;
end C;

// CHECK-NOT: has no valid result
// CHECK-NOT: §6.6
// CHECK-NOT: §25.2
// CHECK: {{.}}
