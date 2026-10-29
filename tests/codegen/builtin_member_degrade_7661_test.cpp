// Issue #7661 (stdlib §2.5 DateTime/TimeSpan/DateOnly/TimeOnly, §2.6 Guid,
// §2.7 Random, §3.1 StringBuilder, v0.350): a member that is NOT on a by-name
// built-in's declared surface was left untyped and unreported by sema, so
// codegen degraded it to a zero value (or an empty statement) and the program
// compiled and RAN behind exit 0. stdlib §2.5 gives DateTime `Ticks` and
// `FromTicks` but no `AddTicks`; stdlib §2.7 lists no `Random.Shared`. Each
// must be a compile error naming the missing member, in every receiver shape
// (annotated local, parameter, `new`, factory result, static type name, type
// alias, `?.`). A same-named user type (a shadow) and a §26.5 extension that
// declares the name keep the access accepted.
//
// `StringBuilder.Capacity` IS declared (§3.1) but the runtime has no
// character capacity to read; it is refused as not-yet-implemented rather
// than degraded (see the row below), pending a ruling on its value.
//
// Oracle: runAot. Reject rows assert the specific diagnostic text; controls
// assert exact stdout.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;

std::string prog(const std::string& body) {
    return "module M;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectRejectSrc(const std::string& src, const char* needle) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

void expectNoMember(const std::string& body, const char* needle) {
    expectRejectSrc(prog(body), needle);
}

void expectStdoutSrc(const std::string& src, const std::string& expected) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

void expectStdout(const std::string& body, const std::string& expected) {
    expectStdoutSrc(prog(body), expected);
}

}  // namespace

// ---- the issue's repros -------------------------------------------------

// stdlib §2.5: DateTime declares no AddTicks (the issue's chain).
BEZEL_TEST(builtin_member_degrade_7661, datetime_addticks_chain_is_no_member_2_5) {
    expectNoMember(
        "    Console.WriteLine(DateTime.FromTicks(5L).AddTicks(3L).Ticks);\n",
        "no member `AddTicks` on built-in type `DateTime`");
}

// stdlib §2.7: Random declares no static Shared.
BEZEL_TEST(builtin_member_degrade_7661, random_shared_is_no_member_2_7) {
    expectNoMember("    var r := Random.Shared;\n"
                   "    Console.WriteLine(r.NextInt64());\n",
                   "no member `Shared` on built-in type `Random`");
}

// ---- receiver shapes (stdlib §2.5) --------------------------------------

BEZEL_TEST(builtin_member_degrade_7661, datetime_parameter_receiver_2_5) {
    expectRejectSrc(
        "module M;\n"
        "public class C\n"
        "  public static method Take(x: DateTime): int\n"
        "    Console.WriteLine(x.Bogus1);\n"
        "    return 0;\n"
        "  end Take;\n"
        "  public static method Main(): int\n"
        "    return C.Take(DateTime.FromTicks(1L));\n"
        "  end Main;\n"
        "end C;\n",
        "no member `Bogus1` on built-in type `DateTime`");
}

BEZEL_TEST(builtin_member_degrade_7661, datetime_new_receiver_2_5) {
    expectNoMember("    var a := new DateTime(2024, 3, 15);\n"
                   "    Console.WriteLine(a.Bogus2);\n",
                   "no member `Bogus2` on built-in type `DateTime`");
}

BEZEL_TEST(builtin_member_degrade_7661, datetime_annotated_receiver_2_5) {
    expectNoMember("    var b: DateTime := DateTime.Now;\n"
                   "    Console.WriteLine(b.Bogus3);\n",
                   "no member `Bogus3` on built-in type `DateTime`");
}

BEZEL_TEST(builtin_member_degrade_7661, datetime_factory_result_receiver_2_5) {
    expectNoMember("    var a := new DateTime(2024, 3, 15);\n"
                   "    var d := a.AddDays(1.0);\n"
                   "    Console.WriteLine(d.Bogus5);\n",
                   "no member `Bogus5` on built-in type `DateTime`");
}

BEZEL_TEST(builtin_member_degrade_7661, datetime_static_receiver_2_5) {
    expectNoMember("    Console.WriteLine(DateTime.Bogus6);\n",
                   "no member `Bogus6` on built-in type `DateTime`");
}

BEZEL_TEST(builtin_member_degrade_7661, datetime_alias_static_call_2_5) {
    expectRejectSrc("module M;\n"
                    "type DT := DateTime;\n"
                    "public class C\n"
                    "  public static method Main(): int\n"
                    "    Console.WriteLine(DT.Bogus7());\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n",
                    "no member `Bogus7` on built-in type `DateTime`");
}

BEZEL_TEST(builtin_member_degrade_7661, dateonly_timeonly_receivers_2_5) {
    expectNoMember("    var d := new DateOnly(2024, 3, 15);\n"
                   "    Console.WriteLine(d.Bogus1);\n",
                   "no member `Bogus1` on built-in type `DateOnly`");
    expectNoMember("    var t := new TimeOnly(1, 2);\n"
                   "    Console.WriteLine(t.Bogus2);\n",
                   "no member `Bogus2` on built-in type `TimeOnly`");
}

// ---- the other closed surfaces ------------------------------------------

BEZEL_TEST(builtin_member_degrade_7661, timespan_factory_call_2_5) {
    expectNoMember("    var t := TimeSpan.FromDays(1.0);\n"
                   "    Console.WriteLine(t.Bogus1(1));\n",
                   "no member `Bogus1` on built-in type `TimeSpan`");
}

BEZEL_TEST(builtin_member_degrade_7661, guid_factory_call_2_6) {
    expectNoMember("    var g := Guid.NewGuid();\n"
                   "    Console.WriteLine(g.Bogus2(1));\n",
                   "no member `Bogus2` on built-in type `Guid`");
}

BEZEL_TEST(builtin_member_degrade_7661, random_method_result_receiver_2_7) {
    expectRejectSrc(
        "module M;\n"
        "public class C\n"
        "  public static method R(): owned Random => new Random(1L);\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(C.R().Bogus6());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `Bogus6` on built-in type `Random`");
}

BEZEL_TEST(builtin_member_degrade_7661, stringbuilder_append_result_3_1) {
    expectNoMember("    var s := new StringBuilder().Append(\"x\");\n"
                   "    Console.WriteLine(s.Bogus3(1));\n",
                   "no member `Bogus3` on built-in type `StringBuilder`");
}

BEZEL_TEST(builtin_member_degrade_7661, array_element_receiver_2_5) {
    expectNoMember("    var arr := new Array<DateTime>(1);\n"
                   "    Console.WriteLine(arr[0].Bogus7);\n",
                   "no member `Bogus7` on built-in type `DateTime`");
}

// StringBuilder.Capacity is declared by §3.1 with no stated value and has no
// lowering: refused, not degraded to an empty statement behind exit 0.
BEZEL_TEST(builtin_member_degrade_7661, stringbuilder_capacity_refused_not_degraded_3_1) {
    expectNoMember("    var sb := new StringBuilder();\n"
                   "    Console.WriteLine(sb.Capacity);\n",
                   "member `Capacity` on `StringBuilder` is declared");
}

// ---- the `?.` form (§7.5) ------------------------------------------------

BEZEL_TEST(builtin_member_degrade_7661, nullcond_receivers_2_5_2_6_2_7_3_1) {
    expectNoMember("    var d: DateTime? := DateTime.FromTicks(5L);\n"
                   "    Console.WriteLine($\"{d?.AddTicks(3L)}\");\n",
                   "no member `AddTicks` on built-in type `DateTime`");
    expectNoMember("    var r: Random? := new Random(1L);\n"
                   "    Console.WriteLine($\"{r?.Shared}\");\n",
                   "no member `Shared` on built-in type `Random`");
    expectNoMember("    var s: StringBuilder? := new StringBuilder();\n"
                   "    Console.WriteLine($\"{s?.Bogus2}\");\n",
                   "no member `Bogus2` on built-in type `StringBuilder`");
    expectNoMember("    var g: Guid? := Guid.Empty;\n"
                   "    Console.WriteLine($\"{g?.Bogus4}\");\n",
                   "no member `Bogus4` on built-in type `Guid`");
}

// Every `?.` read on one time-value receiver is rejected, not every other
// one: the discarded `?.` twin used to be freed, and the next twin reused its
// address and read back the stale receiver type as its own (MEASURED: the
// second of three reads was accepted).
BEZEL_TEST(builtin_member_degrade_7661, nullcond_consecutive_reads_all_rejected_2_5) {
    const std::string src = prog("    var b: TimeSpan? := TimeSpan.Zero;\n"
                                 "    Console.WriteLine($\"{b?.Bogus5}\");\n"
                                 "    Console.WriteLine($\"{b?.Bogus6}\");\n"
                                 "    Console.WriteLine($\"{b?.Bogus7}\");\n");
    expectRejectSrc(src, "no member `Bogus5` on built-in type `TimeSpan`");
    expectRejectSrc(src, "no member `Bogus6` on built-in type `TimeSpan`");
    expectRejectSrc(src, "no member `Bogus7` on built-in type `TimeSpan`");
}

// ---- controls -----------------------------------------------------------

BEZEL_TEST(builtin_member_degrade_7661, declared_members_still_work_2_5_2_7_3_1) {
    expectStdout("    var d := DateTime.FromTicks(5L);\n"
                 "    Console.WriteLine(d.Ticks);\n"
                 "    Console.WriteLine(d.AddDays(1.0).Ticks);\n"
                 "    var t := TimeSpan.FromTicks(7L);\n"
                 "    Console.WriteLine(t.Ticks);\n"
                 "    var r := new Random(42L);\n"
                 "    Console.WriteLine(r.Next(1, 2));\n"
                 "    var sb := new StringBuilder();\n"
                 "    sb.Append(\"ab\");\n"
                 "    Console.WriteLine(sb.Length);\n"
                 "    var n: TimeSpan? := TimeSpan.FromTicks(3L);\n"
                 "    Console.WriteLine($\"{n?.Ticks}\");\n",
                 "5\n864000000005\n7\n1\n2\n3");
}

// A user type named like a built-in is the user's type (its own members).
BEZEL_TEST(builtin_member_degrade_7661, user_shadow_type_is_not_the_builtin) {
    expectStdoutSrc("module M;\n"
                    "public class Random\n"
                    "  public method Bump(): int => 1;\n"
                    "  public static method Shared(): int => 2;\n"
                    "  public property Seed: int => 3;\n"
                    "end Random;\n"
                    "public class C\n"
                    "  public static method Main(): int\n"
                    "    var d := new Random();\n"
                    "    Console.WriteLine(d.Bump());\n"
                    "    Console.WriteLine(Random.Shared());\n"
                    "    Console.WriteLine(d.Seed);\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n",
                    "1\n2\n3");
}

// §26.5: an extension that declares the name keeps the access accepted.
// (A `new` receiver: an extension call on a FACTORY-shaped DateTime /
// TimeSpan / Guid receiver fails in codegen on origin/main, a separate
// defect reported with this fix.)
BEZEL_TEST(builtin_member_degrade_7661, extension_declared_name_still_works_26_5) {
    expectStdoutSrc("module M;\n"
                    "extension DateTime\n"
                    "  public method AddTicks(n: long): long => this.Ticks + n;\n"
                    "end;\n"
                    "extension Random\n"
                    "  public property Shared: int => 3;\n"
                    "end;\n"
                    "public class C\n"
                    "  public static method Main(): int\n"
                    "    var d := new DateTime(2024, 3, 16);\n"
                    "    Console.WriteLine(d.AddTicks(5L) - d.Ticks);\n"
                    "    var r := new Random(1L);\n"
                    "    Console.WriteLine(r.Shared);\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n",
                    "5\n3");
}
