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

// #6680 (§7.5, §4.6, §28.9): `x?.ToString()` on a numeric / char / decimal
// primitive receiver mints a fresh string the plain local solely owns (the `.`
// form was reclaimed via isPlusOneStringProducer). Probe s3_dbl_tostring_nc.
// Looped 100x per receiver kind; stdout asserted (a leak fix must not change it).
BEZEL_TEST(null_cond_builtin_followups, primitive_tostring_local_no_leak_6680_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  var i := 0; var acc := 0; "
        "  while i < 100 do "
        "    var d: double := 1.5; var ds := d?.ToString(); "
        "    var n: int := 7; var ns := n?.ToString(); "
        "    var l: long := 70000000000; var ls := l?.ToString(); "
        "    var c: char := 'a'; var cs := c?.ToString(); "
        "    var m: decimal := 1.25m; var ms := m?.ToString(); "
        "    acc := acc + (ds?.Length ?? 0) + (ns?.Length ?? 0) + (ls?.Length ?? 0) "
        "         + (cs?.Length ?? 0) + (ms?.Length ?? 0); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(acc); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2000\n");
}

// Nullable primitive receiver (`int?`), non-null and null: the null path leaves
// a null string in the slot, which the scope-end release tolerates.
BEZEL_TEST(null_cond_builtin_followups, nullable_primitive_tostring_local_6680_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  var i := 0; var acc := 0; var nulls := 0; "
        "  while i < 100 do "
        "    var a: int? := 7; var as1 := a?.ToString(); "
        "    var b: int? := null; var bs := b?.ToString(); "
        "    acc := acc + (as1?.Length ?? 0); "
        "    if bs = null then nulls := nulls + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(acc); Console.WriteLine(nulls); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n100\n");
}

// #6664 (stdlib §5, §7.5): `a.GroupBy(f)?.ToArray()` never lowered -- the bare
// GroupBy receiver has no standalone value, so lowerLocalDecl dropped the
// initializer (`g` NULL, internal #2434 warning, exit 0): wrong output, not a
// leak. Probe g5. Pre-fix prints "NULL"; the `.` form prints the group count.
BEZEL_TEST(null_cond_builtin_followups, groupby_toarray_local_6664_spec_7_5) {
    JitResult r = runAot(prog(
        "  var a: Array<int> := [1, 2, 3, 4]; "
        "  var g := a.GroupBy(lambda(x: int) => x mod 2)?.ToArray(); "
        "  if g = null then Console.WriteLine(\"NULL\"); "
        "  else Console.WriteLine(g.Count); end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// Owning disposal of the groupings (isFreshGroupByToArrayLocal), 100x checked,
// with the lazy terminals over the same receiver as stdout controls.
BEZEL_TEST(null_cond_builtin_followups, groupby_toarray_local_no_leak_6664_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  var a: Array<int> := [1, 2, 3, 4, 5]; "
        "  var i := 0; var acc := 0; "
        "  while i < 100 do "
        "    var g := a.GroupBy(lambda(x: int) => x mod 2)?.ToArray(); "
        "    if g <> null then acc := acc + g.Count; end; "
        "    acc := acc + (a.GroupBy(lambda(x: int) => x mod 3)?.Count() ?? 0); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(acc); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "500\n");
}
