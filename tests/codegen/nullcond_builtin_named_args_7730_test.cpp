// Spec-derived regression test for issue #7730 -- `x?.Insert(value := V,
// index := I)` on a StringBuilder failed IR verification: the `?.` twin of a
// built-in member call is lowered as a synthetic `.` call, and the named-
// argument binding sema published (`checkBuiltinNamedArgs` ->
// setBuiltinCallBinding, keyed on the CallExpr it checked) was not carried
// onto that synthetic node, so the arm lowered the arguments in parameter
// order against swapped slots (`sext ptr` / `bz_sb_insert(ptr, i64, i32)`).
//
// spec §22.5 "Evaluation order" (v0.350): "A call's supplied argument
// expressions are evaluated in call-site textual order ... naming an argument
// changes which parameter it binds to, never when it runs."
// spec §7.5 "Null-conditional access" (v0.350), Evaluation: "`?.` short-circuits.
// The receiver is evaluated first; if it is null the access does not occur and
// nothing to its right in the chain is evaluated -- ... the argument and index
// expressions of the skipped suffixes are not evaluated either."
// stdlib §3.1 (`StringBuilder.Insert` / `Remove` / `Replace`), v0.350.
//
// Every argument is a call `T(sb, "<tag>", value)` appending its tag to a
// trace; the trace proves "receiver, then A, then B, each exactly once", and
// is just the receiver tag on a null receiver.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

const char* kHelpers =
    "  public static method T(sb: StringBuilder, s: string, v: int): int "
    "    sb.Append(s); "
    "    return v; "
    "  end T; "
    "  public static method TS(sb: StringBuilder, s: string, v: string): string "
    "    sb.Append(s); "
    "    return v; "
    "  end TS; "
    "  public static method Pick(sb: StringBuilder, s: string, t: StringBuilder?): StringBuilder? "
    "    sb.Append(s); "
    "    return t; "
    "  end Pick; ";

std::string prog(const std::string& body) {
    return std::string("module M; import System; import System.Text; "
                       "class C ") +
           kHelpers +
           "  public static method Main(): int "
           "    using var sb := new StringBuilder(); "
           "    using var sb2 := new StringBuilder(); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The issue's repro, positional control: `?.Insert(index, value)`.
BEZEL_TEST(nullcond_builtin_named_args_7730, insert_positional_control) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    sb2.Append(\"abc\"); "
            "    var r := Pick(sb, \"R\", sb2)?.Insert(T(sb, \"B\", 1), TS(sb, \"A\", \"Z\")); "
            "    Console.WriteLine(sb.ToString() + \"|\" + sb2.ToString()); ")),
        std::string("RBA|aZbc"));
}

// The issue's repro: reordered named arguments through `?.`. The value is
// written first, so its tag A precedes B; the receiver R precedes both.
BEZEL_TEST(nullcond_builtin_named_args_7730, insert_named_reordered_written_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    sb2.Append(\"abc\"); "
            "    var r := Pick(sb, \"R\", sb2)?.Insert(value := TS(sb, \"A\", \"Z\"), "
            "                                       index := T(sb, \"B\", 1)); "
            "    Console.WriteLine(sb.ToString() + \"|\" + sb2.ToString()); ")),
        std::string("RAB|aZbc"));
}

// Named arguments in parameter order through `?.` (the identity binding).
BEZEL_TEST(nullcond_builtin_named_args_7730, insert_named_in_parameter_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    sb2.Append(\"abc\"); "
            "    var r := Pick(sb, \"R\", sb2)?.Insert(index := T(sb, \"A\", 2), "
            "                                       value := TS(sb, \"B\", \"Z\")); "
            "    Console.WriteLine(sb.ToString() + \"|\" + sb2.ToString()); ")),
        std::string("RAB|abZc"));
}

// Remove and Replace take the same path through the `?.` twin.
BEZEL_TEST(nullcond_builtin_named_args_7730, remove_and_replace_named_reordered) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    sb2.Append(\"abcdef\"); "
            "    var a := Pick(sb, \"R\", sb2)?.Remove(length := T(sb, \"A\", 1), "
            "                                       startIndex := T(sb, \"B\", 0)); "
            "    var b := Pick(sb, \"S\", sb2)?.Replace(newValue := TS(sb, \"C\", \"Z\"), "
            "                                        oldValue := TS(sb, \"D\", \"c\")); "
            "    Console.WriteLine(sb.ToString() + \"|\" + sb2.ToString()); ")),
        std::string("RABSCD|bZdef"));
}

// A null receiver: `?.` short-circuits, so NEITHER argument runs (only the
// receiver's own tag is traced), the builder is unchanged, and the result is
// null.
BEZEL_TEST(nullcond_builtin_named_args_7730, null_receiver_evaluates_no_argument) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    sb2.Append(\"abc\"); "
            "    var r1 := Pick(sb, \"R\", null)?.Insert(value := TS(sb, \"A\", \"Z\"), "
            "                                         index := T(sb, \"B\", 1)); "
            "    var r2 := Pick(sb, \"S\", null)?.Remove(length := T(sb, \"C\", 1), "
            "                                         startIndex := T(sb, \"D\", 0)); "
            "    var r3 := Pick(sb, \"U\", null)?.Insert(T(sb, \"E\", 1), TS(sb, \"F\", \"Z\")); "
            "    var allNull := 0; "
            "    if r1 is null and r2 is null and r3 is null then allNull := 1; end; "
            "    Console.WriteLine(sb.ToString() + \"|\" + sb2.ToString() + \"|\" + "
            "                      allNull.ToString()); ")),
        std::string("RSU|abc|1"));
}

// 100x under ASan + LSan: the string arguments passed as reordered named
// arguments through `?.`, on both the non-null and the null path, leave no
// leak and no double release. (A +1 string temporary such as "X" + "Y" as the
// Insert value leaks in declaration order through a plain `.` too -- the
// StringBuilder.Insert sibling of issue #7600's handback, so not used here.)
BEZEL_TEST(nullcond_builtin_named_args_7730, named_loop_clean_under_asan) {
    JitResult r = runAotChecked(prog(
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      sb2.Clear(); sb2.Append(\"abc\"); "
        "      var a := Pick(sb, \"R\", sb2)?.Insert(value := TS(sb, \"A\", \"XY\"), "
        "                                         index := T(sb, \"B\", 1)); "
        "      var b := Pick(sb, \"S\", null)?.Insert(value := TS(sb, \"C\", \"XY\"), "
        "                                          index := T(sb, \"D\", 1)); "
        "      if a is not null then total += sb2.Length; end; "
        "      if b is null then total += 1; end; "
        "    end for; "
        "    Console.WriteLine(total.ToString() + \"|\" + sb.Length.ToString() + \"|\" + "
        "                      sb.ToString().Substring(0, 4)); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600|400|RABS\n"));
}
