// fix #7932 (spec §28.11 "`move` on a collection slot", v0.350): "`move
// coll[i]` is admitted on exactly these receivers ... a slot of either owning
// inline-array form, `[N owned]T?` or `[N owned]T` ... No other receiver admits
// it." A PLAIN inline array `[N]T` (no `owned` slot marker) is not among them;
// its slots are copies or borrows. Before the fix `return move Registry.Pool[0];`
// through a `static readonly` non-owning `[2]Res?` compiled and ran, handing
// the caller a second owner of an object the slot's holder still referenced.
//
// Rows:
//   T1  the issue's repro (p02): `move Registry.Pool[0]` through a
//       `static readonly [2]Res?` is a compile error
//   T2  a plain inline-array LOCAL slot: the same error (the rule is the
//       receiver's type, not the static position)
//   T3  a plain inline-array INSTANCE field slot, with a replacing `with`
//   T4  ACCEPT control: an owning `[2 owned]Res?` local slot still takes

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
const char* kRes =
    "module M7932; "
    "class Res "
    "  public Name: string; "
    "  public constructor(n: string) this.Name := n; end constructor; "
    "end Res; ";
}  // namespace

BEZEL_TEST(sema_move_plain_inline_array_slot_7932,
           static_readonly_non_owning_slot_is_rejected_spec_28_11) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static readonly Pool: [2]Res?; "
        "  public method Swap(): Res? "
        "    return move Registry.Pool[0]; "
        "  end Swap; "
        "end Registry; "
        "class P public static method Main() "
        "  using var g := new Registry(); "
        "  var r := g.Swap(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "plain inline array's slot"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "28.11"));
}

BEZEL_TEST(sema_move_plain_inline_array_slot_7932,
           local_plain_inline_array_slot_is_rejected_spec_28_11) {
    auto r = runAot(std::string(kRes) +
        "class P public static method Main() "
        "  var a: [2]Res? := [null, null]; "
        "  var r := move a[0]; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "plain inline array's slot"));
}

BEZEL_TEST(sema_move_plain_inline_array_slot_7932,
           instance_field_plain_slot_replacing_move_is_rejected_spec_28_11) {
    auto r = runAot(std::string(kRes) +
        "class Holder "
        "  public Slots: [2]Res?; "
        "  public method Swap(): owned Res? "
        "    return move this.Slots[0] with new Res(\"n\"); "
        "  end Swap; "
        "end Holder; "
        "class P public static method Main() "
        "  using var h := new Holder(); "
        "  using var t := h.Swap(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "plain inline array's slot"));
}

BEZEL_TEST(sema_move_plain_inline_array_slot_7932,
           owning_inline_array_local_slot_take_still_accepted) {
    auto r = runAot(std::string(kRes) +
        "class P public static method Main() "
        "  var a: [2 owned]Res? := [new Res(\"x\"), new Res(\"y\")]; "
        "  using var t := move a[1]; "
        "  Console.WriteLine(t!.Name); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("y\n"));
}
