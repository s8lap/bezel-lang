// Codegen — discovery sweep #7: char.IsControl (stdlib §9.5) was missing from
// both the sema allowlist and the codegen dispatch, while every sibling
// classification method (IsDigit/IsLetter/IsWhiteSpace/IsUpper/IsLower/
// IsPunctuation) was implemented. char.IsControl('\n') was rejected with
// "type 'char' has no static method or property 'IsControl'". Added it to the
// sema allowlist and mapped it to the C library iscntrl().

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

BEZEL_TEST(char_is_control, newline_is_control_letter_is_not) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var ctrl: bool := char.IsControl('\\n'); "    // true
        "  var notctrl: bool := char.IsControl('a'); "   // false
        "  return (10 if ctrl else 0) + (1 if not notctrl else 0); "  // 11
        "end Main; end C;"), 11);
}

BEZEL_TEST(char_is_control, tab_and_space) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: bool := char.IsControl('\\t'); "   // true (tab is a control char)
        "  var sp: bool := char.IsControl(' '); "    // false (space is not a control char)
        "  return (10 if t else 0) + (1 if not sp else 0); "  // 11
        "end Main; end C;"), 11);
}
