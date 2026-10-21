// Follow-ups to #6529 (spec §7.5, stdlib §4.4 / §5, v0.338/v0.339): the `?.`
// CALL spelling of a built-in / LINQ member. Each row names its issue.
// Value rows are runAot and assert stdout; leak rows are runAotChecked, looped.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string prog(const std::string& body) {
    return "module M; class C "
           "  public static method Main(): int " +
           body + " return 0; end Main; end C;";
}
}  // namespace

// #6681 (§7.5): the sema twin of a built-in `?.` call used to name its placeholder
// receiver after the MEMBER, so an unrelated nullable local of that name made the
// twin fail and the line vanished at exit 0. Probe j1_unrelated_local_First_nullable.
BEZEL_TEST(null_cond_builtin_followups, unrelated_local_named_like_member_6681_spec_7_5) {
    JitResult r = runAot(prog(
        "  var First: Array<int>? := null; "
        "  var a: Array<int>? := [4, 2]; "
        "  Console.WriteLine(a?.First() ?? -1); "
        "  Console.WriteLine(First?.Count() ?? -9); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n-9\n");
}

// #6679 (§7.5, stdlib §5, §28.3): `var d := a?.ToDictionary(..)` is a fresh
// dictionary the plain local solely owns; the `.` form and `using var` already
// reclaimed it. Probe s5_todict_local_nc. Looped 100x; output asserted too.
BEZEL_TEST(null_cond_builtin_followups, todictionary_local_no_leak_6679_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  var i := 0; var acc := 0; "
        "  while i < 100 do "
        "    var a: Array<int>? := [1, 2, 3]; "
        "    var d := a?.ToDictionary(lambda(x: int) => x, lambda(x: int) => x * 2); "
        "    if d <> null then acc := acc + d.Count; end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(acc); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "300\n");
}

// Null receiver: the slot stays null; the scope-end reclaim must tolerate it.
BEZEL_TEST(null_cond_builtin_followups, todictionary_local_null_receiver_6679_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  var i := 0; var hits := 0; "
        "  while i < 100 do "
        "    var a: Array<int>? := null; "
        "    var d := a?.ToDictionary(lambda(x: int) => x, lambda(x: int) => x); "
        "    if d <> null then hits := hits + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(hits); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}
