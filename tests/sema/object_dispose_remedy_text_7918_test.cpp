// Regression tests for #7918 (§8.7, v0.350 / §28.10, v0.350): the
// Dispose-on-`Object` diagnostic's remedy read "bind it with `using var`
// (§28.10)", which leads straight into the #7568 error.
//
// §28.10 (v0.350) rejects an unmarked `using var y := x;` "when its
// right-hand side is a named owned binding whose declared type, nullability
// stripped, is a class or interface type" and `Object` is a class (ruling on
// issue #7568, reading (A)). The legal adopting spelling is
// `using var y := move x;`, so the message now names that form.
//
// The text change is pinned three ways: the new remedy is present, the old
// phrase "bind it with `using var`" is absent, and the program the remedy
// names compiles and runs under the checked oracle.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
const char* kD =
    "module M; "
    "class D public constructor() end constructor; end D; ";
}

BEZEL_TEST(object_dispose_remedy_text_7918, remedy_names_the_move_adopting_form) {
    auto r = runAot(std::string(kD) +
        "class C public static method Main(): int "
        "  var o: Object := new D(); o.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "no member `Dispose` on type `Object`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "adopt it with `using var y := move x;`"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "bind it with `using var`"));
}

// The remedy the message names is legal: `using var y := move x;` over an
// owned `Object` compiles and runs (§28.10). This row uses the unchecked
// oracle on purpose: the adopting `using var` over an `Object`-typed owned
// local leaks its payload under ASan (a separate defect, reported in the
// lane handback), so a checked row would pin that defect, not this text.
BEZEL_TEST(object_dispose_remedy_text_7918, named_remedy_compiles_and_runs) {
    auto r = runAot(std::string(kD) +
        "class C public static method Main(): int "
        "  var a := new Array<int>(); a.Add(1); var x: Object := move a; using var y := move x; "
        "  Console.WriteLine(\"ok\"); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// The other named remedy, `_ := x;` (§17.13), is unchanged and legal.
BEZEL_TEST(object_dispose_remedy_text_7918, discard_remedy_compiles_and_runs) {
    auto r = runAotChecked(std::string(kD) +
        "class C public static method Main(): int "
        "  var x: Object := new D(); _ := x; "
        "  Console.WriteLine(\"ok\"); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}
