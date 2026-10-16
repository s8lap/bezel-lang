// Codegen — issue #6313 (§27.5 Closures, §17.6 increment statement, spec
// v0.338): a `var int` captured and mutated ONLY through `n++` by a
// non-escaping lambda bound to a local leaked its 4-byte capture cell on every
// call. §27.5: a captured local's cell is reclaimed when the last holder
// releases it.
//
// Cause: the capture-symbol walker used by collectLambdaCaptures
// (LlvmBackend_Setup.cpp, `walkStmt`) had arms for AssignmentStmt and the other
// statement kinds but none for IncrementStmt, so a lambda whose only reference
// to the captured `var` was `n++` contributed no symbol; the already-boxed cell
// was never classified and cellFreeEligible_ never armed. `n := n + 1` was
// clean, `n++` leaked 400 B / 100 allocations over a 100-call loop.
//
// These rows need the ASan/LSan oracle (`runAotChecked`): the defect is memory
// nobody frees, which a plain build reports as a correct answer. Each row loops
// 200 times. per §27.5 (issue #6313)

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
std::string wrap(const std::string& tBody) {
    return "module M; "
           "public class P "
           "  public static method T(k: int): int " + tBody + " end T; "
           "  public static method Main(): int "
           "    var s := 0; "
           "    for var i in 0 ..< 200 do s := s + T(1); end for; "
           "    Console.WriteLine(s); "
           "    return 0; "
           "  end Main; "
           "end P;";
}
}  // namespace

// The issue's repro.
BEZEL_TEST(lambda_increment_capture_cell_reclaim_6313, issue_repro_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(wrap(
        "var n: int := 0; var mut := lambda() => do n++; end; mut(); return 1;")),
        "200\n");
}

// Variant: an early return taken before the call.
BEZEL_TEST(lambda_increment_capture_cell_reclaim_6313, early_return_before_call_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(wrap(
        "var n: int := 0; var mut := lambda() => do n++; end; "
        "if k > 0 then return 2; end if; mut(); return 1;")),
        "400\n");
}

// Variant: read `n` after the call (value verified: one increment each).
BEZEL_TEST(lambda_increment_capture_cell_reclaim_6313, read_after_call_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(wrap(
        "var n: int := 0; var mut := lambda() => do n++; end; mut(); mut(); return n;")),
        "400\n");
}

// Variant: a `case` arm calls the lambda; `--` too.
BEZEL_TEST(lambda_increment_capture_cell_reclaim_6313, case_arm_calls_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(wrap(
        "var n: int := 10; var up := lambda() => do n++; end; "
        "var dn := lambda() => do n--; end; "
        "case k of 1 => up(), 2 => dn(), _ => dn() end; "
        "return n;")),
        "2200\n");
}
