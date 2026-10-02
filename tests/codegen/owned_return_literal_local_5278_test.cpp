// #5278 — a collection-literal local (`var d: T := [..]`, never reassigned)
// returned through an `owned` return type. Spec: §28.3 Producers table
// ("Collection literal targeting a reference type -> Owned"), §15.4 ("a
// reference-typed collection literal is a fresh owned production"), K3.13
// (`return` of a named owned binding under an `owned` return type is a
// marker-free transfer). Loop-amplified 200x under runAotChecked so the
// `--full` ASan leg pins the free-exactly-once behaviour.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

std::string loopProg(const std::string& type, const std::string& lit,
                     const std::string& use) {
    return "module M; class C "
           "  public static method Make(): owned " + type + " "
           "    var d: " + type + " := " + lit + "; "
           "    return d; "
           "  end Make; "
           "  public static method Main(): int "
           "    var total := 0; "
           "    var i := 0; "
           "    while i < 200 do "
           "      using var got := C.Make(); "
           "      total := total + " + use + "; "
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(total); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

std::string ownProg(const std::string& type, const std::string& lit,
                    const std::string& retStmt) {
    return "module M; class C "
           "  public static method Make(): owned " + type + " "
           "    var d: " + type + " := " + lit + "; "
           "    var e: " + type + " := " + lit + "; "
           "    " + retStmt + " "
           "  end Make; "
           "end C;";
}

}  // namespace

BEZEL_TEST(owned_return_literal_local_5278, array_int_literal_local_owned_return_loop_spec_28_3_K3_13) {
    auto r = runAotChecked(loopProg("Array<int>", "[1, 2, 3]", "got.Count + got[0]"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("800") != std::string::npos);
}

BEZEL_TEST(owned_return_literal_local_5278, array_int_literal_local_owned_return_accepted_spec_28_3_K3_13) {
    auto r = parseAndCheckOwnership(ownProg("Array<int>", "[1, 2, 3]", "return d;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owned_return_literal_local_5278, dictionary_string_int_literal_local_owned_return_loop_spec_28_3_K3_13) {
    auto r = runAotChecked(loopProg("Dictionary<string, int>", "[\"a\": 1, \"b\": 2]", "got.Count"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("400") != std::string::npos);
}

BEZEL_TEST(owned_return_literal_local_5278, dictionary_string_int_literal_local_owned_return_accepted_spec_28_3_K3_13) {
    auto r = parseAndCheckOwnership(ownProg("Dictionary<string, int>", "[\"a\": 1, \"b\": 2]", "return d;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owned_return_literal_local_5278, set_int_literal_local_owned_return_loop_spec_28_3_K3_13) {
    auto r = runAotChecked(loopProg("Set<int>", "[1, 2, 3, 4]", "got.Count"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("800") != std::string::npos);
}

BEZEL_TEST(owned_return_literal_local_5278, set_int_literal_local_owned_return_accepted_spec_28_3_K3_13) {
    auto r = parseAndCheckOwnership(ownProg("Set<int>", "[1, 2, 3, 4]", "return d;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `move` on a `return` operand under an `owned` return type is the redundant
// marker (K3.13 marker economy, v0.309) — the ONLY error; it is no longer
// also reported as a borrow.
BEZEL_TEST(owned_return_literal_local_5278, move_variant_reports_only_redundant_marker_spec_28_3_K3_13) {
    auto r = parseAndCheckOwnership(ownProg("Array<int>", "[1, 2, 3]", "return move d;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "the `move` marker is redundant on a `return` operand"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, "cannot return a borrow as an `owned` value"));
}

// A literal local that is REASSIGNED may hold a borrow: still rejected.
BEZEL_TEST(owned_return_literal_local_5278, reassigned_literal_local_still_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Make(a: Array<int>): owned Array<int> "
        "    var d: Array<int> := [1, 2, 3]; "
        "    d := a; "
        "    return d; "
        "  end Make; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "a borrow cannot be returned as an `owned` value"));
}

// Control: a literal local returned through a PLAIN instance return is handled
// as before this fix (accepted — unchanged by #5278).
BEZEL_TEST(owned_return_literal_local_5278, plain_instance_return_of_literal_local_unchanged) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Make(): Array<int> "
        "    var d: Array<int> := [1, 2, 3]; "
        "    return d; "
        "  end Make; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
