// Regression for #7732 -- `Path.Combine(b := "y", a := "x")` was accepted and
// bound BY POSITION (printed "y/x").
//
// per spec §22.5 (v0.350): a named argument binds the parameter with that name,
// in any order after positional ones. stdlib §7.4 (v0.350) declares
// `Path.Combine(a: string, b: string)` and `GetFileName(path: string)` etc.,
// so the names are declared: the call binds by name, an unknown name is
// rejected.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string prog(const std::string& body) {
    return "module M; class P public static method Main() " + body +
           " end Main; end P;";
}
}  // namespace

BEZEL_TEST(path_combine_named_args_7732, reordered_names_bind_by_name) {
    auto r = runAot(prog("Console.WriteLine(Path.Combine(b := \"y\", a := \"x\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x/y\n"));
}

BEZEL_TEST(path_combine_named_args_7732, in_order_names_and_mixed_still_work) {
    auto r = runAot(prog(
        "Console.WriteLine(Path.Combine(a := \"x\", b := \"y\")); "
        "Console.WriteLine(Path.Combine(\"p\", b := \"q\")); "
        "Console.WriteLine(Path.GetFileName(path := \"d/f.txt\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x/y\np/q\nf.txt\n"));
}

BEZEL_TEST(path_combine_named_args_7732, unknown_name_rejected) {
    auto r = runAot(prog("Console.WriteLine(Path.Combine(a := \"x\", c := \"y\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

BEZEL_TEST(path_combine_named_args_7732, wrong_name_on_single_param_member_rejected) {
    auto r = runAot(prog("Console.WriteLine(Path.GetFileName(file := \"d/f.txt\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

BEZEL_TEST(path_combine_named_args_7732, positional_unchanged) {
    auto r = runAot(prog("Console.WriteLine(Path.Combine(\"x\", \"y\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x/y\n"));
}

// §22.5 (v0.350): supplied arguments are evaluated in CALL-SITE textual order,
// whichever parameter they bind.
BEZEL_TEST(path_combine_named_args_7732, reordered_names_evaluate_in_textual_order) {
    auto r = runAot(
        "module M; class P "
        "public static method F(s: string): string Console.WriteLine(\"eval \" + s); return s; end F; "
        "public static method Main() "
        "Console.WriteLine(Path.Combine(b := F(\"y\"), a := F(\"x\"))); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eval y\neval x\nx/y\n"));
}
