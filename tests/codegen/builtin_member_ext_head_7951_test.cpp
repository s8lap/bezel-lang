// Issue #7951 (§26.5 rule 2, v0.350; stdlib §2.5 DateTime, §2.7 Random):
// the closed-member-surface check of #7661 excused an undeclared member of a
// by-name built-in whenever ANY extension block declared a member of that
// NAME. §26.5 rule 2 searches only the blocks "on x's type, base types, or
// implemented interfaces", so a block whose head is another type
// (`extension int { property Shared }`) is no candidate for a `Random`
// receiver and must not excuse `random.Shared`.
//
// (N1) a non-applicable head no longer excuses the name, in the `.` and `?.`
//      forms; an applicable head (the built-in itself) still does.
// (N2) an extension PROPERTY an imported module declares on a built-in head is
//      not carried across the module boundary (a different defect, issue
//      #7952), so the refusal must not claim that no extension declares the
//      name: it says the imported property is not visible.
// (N3) the ambiguity error of fix #5140 (§2.3, v0.309) on a bare head is not
//      followed by a second "no member" error on the same access.
//
// Oracle: runAot / runAotModules. Reject rows assert the specific diagnostic.

#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;
using bezel::test::runAotModules;

std::string withExt(const std::string& ext, const std::string& body) {
    return "module M;\n" + ext +
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectNoMember(const std::string& ext, const std::string& body,
                    const char* needle) {
    JitResult r = runAot(withExt(ext, body));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

const char* kRandomShared = "no member `Shared` on built-in type `Random`";

}  // namespace

// ---- N1: the head decides, not the member name ---------------------------

// The issue's repro n22: `extension int` declares `Shared`; `r?.Shared` on a
// Random? is still no member of Random (stdlib §2.7).
BEZEL_TEST(builtin_member_ext_head_7951, int_head_does_not_excuse_nullcond_26_5) {
    expectNoMember("extension int\n"
                   "  public property Shared: int => 3;\n"
                   "end;\n",
                   "    var r: Random? := new Random(1L);\n"
                   "    Console.WriteLine($\"[{r?.Shared}]\");\n",
                   kRandomShared);
}

// The issue's repro n20: `extension string`, the `.` form.
BEZEL_TEST(builtin_member_ext_head_7951, string_head_does_not_excuse_dot_26_5) {
    expectNoMember("extension string\n"
                   "  public property Shared: int => 3;\n"
                   "end;\n",
                   "    var r := new Random(1L);\n"
                   "    Console.WriteLine(r.Shared);\n",
                   kRandomShared);
}

// Another built-in's head (`extension DateTime`) is no candidate for a
// Random receiver either; static spelling, method form.
BEZEL_TEST(builtin_member_ext_head_7951, other_builtin_head_does_not_excuse_26_5) {
    expectNoMember("extension DateTime\n"
                   "  public method Pick(): int => 5;\n"
                   "end;\n",
                   "    var r := new Random(1L);\n"
                   "    Console.WriteLine(r.Pick());\n",
                   "no member `Pick` on built-in type `Random`");
    expectNoMember("extension TimeSpan\n"
                   "  public property Shared: int => 3;\n"
                   "end;\n",
                   "    Console.WriteLine(Random.Shared);\n",
                   kRandomShared);
}

// A user class head is not applicable to a Random either.
BEZEL_TEST(builtin_member_ext_head_7951, user_class_head_does_not_excuse_26_5) {
    expectNoMember("public class Widget\n"
                   "  public constructor() end constructor;\n"
                   "end Widget;\n"
                   "extension Widget\n"
                   "  public property Shared: int => 3;\n"
                   "end;\n",
                   "    var r := new Random(1L);\n"
                   "    Console.WriteLine(r.Shared);\n",
                   kRandomShared);
}

// An interface the built-in implements (stdlib §2.5: DateTime is
// `IComparable<DateTime>`) is an applicable head, so its member name is not
// refused as "no extension declares it". (Whether the member then dispatches
// is a separate matter; this row pins only that the closed-surface refusal
// does not fire.)
BEZEL_TEST(builtin_member_ext_head_7951, implemented_interface_head_is_not_refused_26_5) {
    JitResult r = runAot(withExt(
        "extension IComparable<DateTime>\n"
        "  public property Cmp: int => 4;\n"
        "end;\n",
        "    var d := new DateTime(2024, 3, 16);\n"
        "    Console.WriteLine(d.Cmp);\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "no member `Cmp`"));
}

// Control: the built-in's own head still excuses its name, in both forms.
BEZEL_TEST(builtin_member_ext_head_7951, own_head_still_excuses_26_5) {
    JitResult r = runAot(withExt(
        "extension Random\n"
        "  public property Shared: int => 3;\n"
        "end;\n",
        "    var r := new Random(1L);\n"
        "    Console.WriteLine(r.Shared);\n"
        "    var n: Random? := r;\n"
        "    Console.WriteLine($\"{n?.Shared}\");\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n3\n");
}

// ---- N2: an imported extension property on a built-in head ----------------

// The refusal must not say that no extension declares the name when an
// imported block does (issue #7952 is why the property cannot be read).
BEZEL_TEST(builtin_member_ext_head_7951, imported_builtin_property_refusal_is_not_false_26_5) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib",
                    "module Lib;\n"
                    "public extension DateTime\n"
                    "  public property Tick2: long => this.Ticks * 2L;\n"
                    "end;\n"});
    mods.push_back({"App",
                    "module App;\n"
                    "import Lib;\n"
                    "public class C\n"
                    "  public static method Main(): int\n"
                    "    var d := new DateTime(2024, 3, 16);\n"
                    "    Console.WriteLine(d.Tick2);\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n"});
    JitResult r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Tick2` on built-in type `DateTime`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension property that an imported module declares on a "
           "built-in type is not visible to this compiler"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "no \xC2\xA7""26.5 extension declares a member named"));
}

// Cross-module N1: an imported `extension int` property named `Shared` does
// not excuse `Random.Shared`, and carries no "imported property" note (the
// imported block's head does not apply to a Random).
BEZEL_TEST(builtin_member_ext_head_7951, imported_int_head_does_not_excuse_26_5) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib",
                    "module Lib;\n"
                    "public extension int\n"
                    "  public property Shared: int => this * 2;\n"
                    "end;\n"});
    mods.push_back({"App",
                    "module App;\n"
                    "import Lib;\n"
                    "public class C\n"
                    "  public static method Main(): int\n"
                    "    var r := new Random(1L);\n"
                    "    Console.WriteLine(r.Shared);\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n"});
    JitResult r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRandomShared));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "declares on a built-in type is not visible"));
}

// ---- N3: no cascade after the #5140 ambiguity error ------------------------

BEZEL_TEST(builtin_member_ext_head_7951, no_cascade_after_ambiguity_error_2_3) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib",
                    "module Lib;\n"
                    "public class DateTime\n"
                    "  public static method Zap(): int => 9;\n"
                    "end DateTime;\n"});
    mods.push_back({"App",
                    "module App;\n"
                    "import Lib;\n"
                    "public class C\n"
                    "  public static method Main(): int\n"
                    "    Console.WriteLine(DateTime.Zap());\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n"});
    JitResult r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "bare name `DateTime` is ambiguous"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "no member `Zap`"));
}
