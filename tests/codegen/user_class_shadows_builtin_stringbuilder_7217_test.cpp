// Regression tests for #7217 and #6845 — the codegen claimed a USER-DECLARED
// class named `StringBuilder` by HEAD NAME, and the builtin's `bz_sb_*`
// runtime ran on the user's object.
//
// WHAT THE DOCUMENTS REQUIRE.  §2.3 (v0.345), the always-available-names
// table: the `System.Text` row lists `StringBuilder` (stdlib §3.1) as
// SHADOWABLE, and §2.2 makes a current-module declaration shadow an
// auto-imported name.  So a user's own `class StringBuilder` is a DISTINCT
// type whose own members resolve; the built-in stays reachable under its
// qualified spelling (`System.Text.StringBuilder`).
//
// THE DEFECT.  Two sites decided "is this receiver the builtin
// `StringBuilder`?" by comparing the receiver's HEAD NAME against the
// literal "StringBuilder":
//
//   * `Emitter::tryLowerCollectionInstanceCall`'s `isStrBld` (the whole
//     builtin `Append` / `AppendLine` / `Insert` / `Remove` / `Replace` /
//     `Clear` arm), and
//   * `Emitter::lowerExprInner`'s `isSB` (the `StringBuilder.Length` sum).
//
// A user class declared `StringBuilder` has that head name, so the builtin's
// arm claimed the user's object.  The two symptoms are ONE cause:
//
//   * #7217 — the user's OWN method was SILENTLY DROPPED.  The program
//     compiled, ran, and printed only "after"; the user's `Replace` was
//     never called.  A silently dropped call is worse than a crash for a
//     program that reads its output.
//   * #6845 — the builtin's `bz_sb_*` runtime ran on the user's object and
//     SEGV'd.  Measured under generated-code ASan: `SEGV ... in
//     bz_sb_content_cps ... bz_sb_replace`.
//
// THE FIX.  The discriminator is the receiver's SYMBOL, not its head name:
// `Emitter::builtinTypeName` is empty for a user type and for an IMPORTED
// user type, and it sees through a transparent type alias (#1462 / §27.8)
// and the `Buffer` -> `__Buffer` intrinsic rename (#5390 / §2.3).
//
// THE ORACLE.  A dropped call prints NOTHING, so the absence of a
// diagnostic proves nothing (and `runAot` fills `r.diagnostics` only on a
// reject — asserting its absence on a program that compiles is VACUOUS).
// Every row here therefore asserts the program's EXACT stdout, that it RAN,
// and its EXIT CODE.  The control rows are load-bearing: the REAL builtin
// `StringBuilder` and every member of it must keep working exactly as
// before, and the same user class under a DIFFERENT name must have worked
// all along (which is what proves the program is well-formed and the
// difference is the name).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The user class, spelled `StringBuilder`.  `Replace` and `Insert` are the
// two members the builtin arm claimed (#7217's dropped call and #6845's
// SEGV respectively); `Length` is the member the Exprs-side arm claimed.
const char* kUserStringBuilder =
    "class StringBuilder "
    "  public method Replace(a: string, b: string): StringBuilder "
    "    Console.WriteLine(\"USER-REPLACE\"); return this; "
    "  end Replace; "
    "  public method Insert(i: int, s: string): StringBuilder "
    "    Console.WriteLine(\"USER-INSERT\"); return this; "
    "  end Insert; "
    "  public readonly property Length: int := 7; "
    "end StringBuilder; ";

// The SAME class under a name that is not a built-in.  This is the control:
// it must behave identically on the pre-fix compiler and on this one, so a
// failure of a row above cannot be blamed on the program being ill-formed.
const char* kUserMyBuilder =
    "class MyBuilder "
    "  public method Replace(a: string, b: string): MyBuilder "
    "    Console.WriteLine(\"USER-REPLACE\"); return this; "
    "  end Replace; "
    "  public method Insert(i: int, s: string): MyBuilder "
    "    Console.WriteLine(\"USER-INSERT\"); return this; "
    "  end Insert; "
    "  public readonly property Length: int := 7; "
    "end MyBuilder; ";

std::string prog(const char* userClass, const std::string& body) {
    return std::string("module M; ") + userClass +
           " class C public static method Main(): int " + body +
           "  return 0; end Main; end C;";
}

}  // namespace

// #7217 (§2.3 / §2.2, v0.345): a user class named `StringBuilder` keeps its
// OWN `Replace`.  PRE-FIX the user's call was SILENTLY DROPPED and the
// program printed only "after"; the post-fix stdout is "USER-REPLACE" then
// "after".
BEZEL_TEST(user_class_shadows_builtin_stringbuilder_7217,
           spec_12_user_class_named_stringbuilder_replace_is_called) {
    auto r = runAot(prog(kUserStringBuilder,
                         "  var sb: StringBuilder := new StringBuilder(); "
                         "  _ := sb.Replace(\"l\", \"L\"); "
                         "  Console.WriteLine(\"after\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "USER-REPLACE\nafter\n");
}

// #6845 (§2.3 / §2.2, v0.345): the SAME class, `Insert` — the member that
// SEGV'd, because the builtin's `bz_sb_insert` / `bz_sb_content_cps` ran on
// the user's object.  POST-FIX the user's own `Insert` is called and the
// program runs to completion.
BEZEL_TEST(user_class_shadows_builtin_stringbuilder_7217,
           spec_12_user_class_named_stringbuilder_insert_is_called) {
    auto r = runAot(prog(kUserStringBuilder,
                         "  var sb: StringBuilder := new StringBuilder(); "
                         "  _ := sb.Insert(0, \"x\"); "
                         "  Console.WriteLine(\"after\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "USER-INSERT\nafter\n");
}

// #6845's SECOND shape (§2.3 / §2.2, v0.345): the `Insert` is not an instance
// member but a user EXTENSION on the user's own `StringBuilder` class
// (§26.4).  The extension's receiver is the user's class, so the builtin arm
// claimed it here too — the issue's original symptom was "IR verification
// failed" on `bz_sb_insert`, the later measurement a SEGV.
BEZEL_TEST(user_class_shadows_builtin_stringbuilder_7217,
           spec_12_user_extension_on_named_stringbuilder_insert_is_called) {
    auto r = runAot(
        "module M; "
        "class StringBuilder public readonly property Length: int := 7; end StringBuilder; "
        "extension StringBuilder "
        "  public method Insert(i: int, s: string): StringBuilder "
        "    Console.WriteLine(\"USER-EXT-INSERT\"); return this; "
        "  end Insert; "
        "end; "
        "class C public static method Main(): int "
        "  var sb: StringBuilder := new StringBuilder(); "
        "  _ := sb.Insert(0, \"x\"); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "USER-EXT-INSERT\nafter\n");
}

// The Exprs-side arm: `StringBuilder.Length` is a PROPERTY of the user's
// class here, so the user's own member answers it.  Pre-fix the builtin arm
// summed the user's object as a parts-list (`bz_array_len` over a foreign
// pointer).  The user's `Length` is 7 — a value the builtin's parts
// sum can never produce, so the row discriminates the two arms.
BEZEL_TEST(user_class_shadows_builtin_stringbuilder_7217,
           spec_12_user_class_named_stringbuilder_length_is_its_own) {
    auto r = runAot(prog(kUserStringBuilder,
                         "  var sb: StringBuilder := new StringBuilder(); "
                         "  Console.WriteLine(sb.Length); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// CONTROL (the row that proves the program is well-formed): the same class
// under a name that is not a built-in behaves identically — before the fix
// and after it.
BEZEL_TEST(user_class_shadows_builtin_stringbuilder_7217,
           spec_12_control_differently_named_class_is_unaffected) {
    auto r = runAot(prog(kUserMyBuilder,
                         "  var sb: MyBuilder := new MyBuilder(); "
                         "  _ := sb.Replace(\"l\", \"L\"); "
                         "  _ := sb.Insert(0, \"x\"); "
                         "  Console.WriteLine(sb.Length); "
                         "  Console.WriteLine(\"after\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     "USER-REPLACE\nUSER-INSERT\n7\nafter\n");
}

// CONTROL (the row that proves the fix did not BREAK the built-in): the REAL
// builtin `StringBuilder` and every member of it — `Append(string)`,
// `Append(int)`, `Append(char)`, `ToString`, `Length`, `Insert`,
// `Remove`, `Replace`, `AppendLine`, `Clear` — must behave exactly as
// before the fix.  stdlib §3.1 (v0.345) declares that surface.
//
// The expected stdout below is the PRE-FIX stdout: the builtin's behaviour is
// NOT changed by this fix, and a difference here would mean the fix traded a
// wrong answer for a wrong answer.
BEZEL_TEST(user_class_shadows_builtin_stringbuilder_7217,
           spec_3_1_builtin_stringbuilder_members_still_work) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var sb := new StringBuilder(); "
        "  _ := sb.Append(\"ab\"); "
        "  _ := sb.Append(1); "
        "  _ := sb.Append('c'); "
        "  Console.WriteLine(sb.ToString()); "
        "  Console.WriteLine(sb.Length); "
        "  _ := sb.Insert(0, \"Z\"); "
        "  Console.WriteLine(sb.ToString()); "
        "  _ := sb.Remove(0, 1); "
        "  Console.WriteLine(sb.ToString()); "
        "  _ := sb.Replace(\"Z\", \"Q\"); "
        "  Console.WriteLine(sb.ToString()); "
        "  _ := sb.AppendLine(); "
        "  Console.WriteLine(\"[\" + sb.ToString() + \"]\"); "
        "  _ := sb.Clear(); "
        "  Console.WriteLine(\"[\" + sb.ToString() + \"]\"); "
        "  Console.WriteLine(sb.Length); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     "ab1c\n"
                     "4\n"
                     "Zab1c\n"
                     "ab1c\n"
                     "ab1c\n"
                     "[ab1c\n"
                     "]\n"
                     "[]\n"
                     "0\n");
}
