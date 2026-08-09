// fix #3025 — the two AssignmentStmt null-RHS exits left outside #2961.
//
// §6.6 / §7.7 / §28.3 / §28.9, v0.240. The dedicated indexer-setter arm
// and the null branch of `??=` both evaluate an assignment RHS as one full
// expression (§28.3 K8). A §6.9 ChannelSender<int>.Send call returns unit, so
// it has no lowered LLVM value; before #3025 each arm kept the target's prior
// value without any diagnostic. The sibling lit test
// `assignment_rhs_lowering_residual_warns_3025.bzl` is the fail-before/pass-
// after diagnostic pin. This AOT control independently proves the degradation
// remains bounded and the ordinary non-null RHS paths still store correctly.
//
// The `??=` case deliberately uses a nullable LHS. §7.7 defines the left
// operand as T?, and only that shape reaches lowerStmt's RHS-lowering null
// branch. A non-nullable `int` LHS exits before lowering the RHS and cannot
// reproduce this site.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

BEZEL_TEST(codegen_assignment_rhs_residual_3025,
           indexer_and_coalesce_null_rhs_retain_targets_spec_6_6_28_3_28_9) {
    auto r = runAot(
        "module N3025; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var a := new Array<int>(1, 17); "
        "    var x: int? := null; "
        "    using var ch := new Channel<int>(); "
        "    using var s := ch.NewSender(); "
        "    a[0] := s.Send(42); "
        "    x ??= s.Send(43); "
        "    if a[0] = 17 then acc := acc + 1; end; "
        "    if x = null then acc := acc + 2; end; "
        "    a[0] := 23; "
        "    x ??= 7; "
        "    if a[0] = 23 then acc := acc + 4; end; "
        "    if (x ?? -1) = 7 then acc := acc + 8; end; "
        "    if ch.Count = 2 then acc := acc + 16; end; "
        "    return acc; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 31);
}
