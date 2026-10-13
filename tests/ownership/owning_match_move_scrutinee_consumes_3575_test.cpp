// fix-forward #3575 (§35.5 form 2 / §28.3 K3.4 / K7.4, v0.338): the marked
// owning-match scrutinee `case move a of …` CONSUMES the named owning-choice
// local `a`. This file is the RUN-BEHAVIOUR half; the warning-present/absent
// polarity lives in the lit rows
//   tests/lit/ownership/owning_match_move_scrutinee_consumes_3575_silent.bzl
//   tests/lit/ownership/owning_match_borrowing_scrutinee_still_warns_3575.bzl
//   tests/lit/ownership/owning_match_move_scrutinee_one_branch_3575.bzl
// because `runAot` carries diagnostics only on a reject.
//
// THE RULE. §35.5: "the consumed binding is left **moved-from** when the
// construct completes". A moved-from binding has no scope-end drop, so (a) the
// payload must be disposed EXACTLY ONCE -- by the arm that adopted it -- and
// (b) a second `move a` after the match is a use-after-consume and is rejected.
// Both loops run 60 times so a one-shot leak or a double free cannot hide, and
// runAotChecked is always ASan/LSan, so a leak fails `ran`.
//
// per §35.5, §28.3 K3.4 / K7.4, §28.8.1, v0.338, #3575

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kDecls =
    "class Resource "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method N(): int => _n; "
    "  destructor Console.WriteLine(\"disposed\"); end destructor; "
    "end Resource; "
    "choice Bundle "
    "  Ready(owned r: Resource), "
    "  Empty; "
    "end Bundle; ";

std::string prog(const char* body) {
    return std::string("module M; ") + kDecls +
           "class C "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end C;";
}

int countOf(const std::string& hay, const char* needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + 1))
        ++n;
    return n;
}

}  // namespace

BEZEL_TEST(owning_match_move_scrutinee_consumes_3575,
           spec_35_5_case_move_consumes_the_scrutinee_payload_disposed_once) {
    // Looped 60x. The arm adopts the payload (`using var`), so each iteration
    // prints exactly one "disposed" and no second one at the iteration's end.
    auto r = runAotChecked(prog(
        "for var i in 1..60 do "
        "  var a := Bundle.Ready(new Resource(i)); "
        "  case move a of "
        "    Ready(var p) => do using var u := move p; Console.WriteLine(\"arm\"); end, "
        "    Empty => Console.WriteLine(\"empty\") "
        "  end; "
        "end for; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOf(r.stdoutText, "arm"), 60);
    BEZEL_EXPECT_EQ(countOf(r.stdoutText, "disposed"), 60);
}

BEZEL_TEST(owning_match_move_scrutinee_consumes_3575,
           spec_35_5_case_move_on_one_branch_disposes_once_per_path) {
    // §28.3 rule 4: maybe-consumed => "scope-end disposal is per-path". Even
    // iterations take the `case move` branch (the arm adopts and disposes);
    // odd ones do not, and `a` is dropped at scope end. Either way exactly one
    // "disposed" per iteration: 60 total, no leak (LSan) and no double free.
    auto r = runAotChecked(prog(
        "for var i in 1..60 do "
        "  var a := Bundle.Ready(new Resource(i)); "
        "  if i mod 2 = 0 then "
        "    case move a of "
        "      Ready(var p) => do using var u := move p; end, "
        "      Empty => () "
        "    end; "
        "  end; "
        "end for; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOf(r.stdoutText, "disposed"), 60);
}

BEZEL_TEST(owning_match_move_scrutinee_consumes_3575,
           spec_35_5_use_after_case_move_is_rejected) {
    // The consumed binding is moved-from after the construct: a second
    // transfer is rejected with the §28.3 already-moved-out diagnostic.
    // MEASURED before the fix: accepted silently (the binding stayed Owned),
    // i.e. a double consume of one owning value.
    auto r = runAot(prog(
        "var a := Bundle.Ready(new Resource(1)); "
        "case move a of "
        "  Ready(var p) => Console.WriteLine(\"r\"), "
        "  Empty => Console.WriteLine(\"e\") "
        "end; "
        "var z := move a; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "was already moved out"));
}
