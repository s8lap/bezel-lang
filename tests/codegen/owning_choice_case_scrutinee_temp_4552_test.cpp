// Codegen regression tests for issue #4552 (the second of its four OMITTED
// disposals): §35.5 *The owning match*, spec v0.306.
//
// §35.5: "The scrutinee temporary's full expression is the **whole `case`
// construct** — K8.3, applied unchanged — so payloads not transferred into
// bindings (discarded slots like `Fetched(_)`, and the payloads of the value
// when a `_` or non-destructuring arm is selected) are disposed when the
// construct completes, by the same unbound-temporary pass that disposes the
// container. An exception out of an arm disposes the not-yet-consumed
// remainder during unwinding (K3.15)."
//
// THE BUG. The #1795 K8 case-scrutinee reclaim covers a heap class/interface
// or an owning-collection scrutinee: both are POINTERS, and both its ownership
// gate and its codegen gate say so. An owning choice is a move-only VALUE
// type, so the scrutinee lowers to a struct in a register, every gate declined
// it, and the container was owned by nothing — rule-corpus case s35_5_0006
// wanted `hit / widget disposed / after` and observed `hit / after`.
//
// THE NARROWING, which is load-bearing and has its own row below. §35.5's
// owning match TRANSFERS the selected case's owned payloads into the arm's
// bindings, and its own adoption spelling (`using var u := move r;` inside a
// `do … end` arm) disposes the payload at the arm's scope end. Disposing the
// container wholesale at construct completion double-frees exactly that shape
// — measured as a SIGSEGV on rule-corpus case s35_5_0002 while this fix was
// being written. The reclaim is therefore confined to constructs whose arms
// bind nothing, which is the set §35.5's sentence above enumerates.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Widget / Outcome / Client: §35.5's own worked shell, with a destructor that
// PRINTS so the assertions pin the moment of disposal as well as the fact.
const char* kFetchDecls =
    "class Widget "
    "  public constructor() end constructor; "
    "  public method Ping() => Console.WriteLine(\"ping\"); "
    "  destructor Console.WriteLine(\"widget disposed\"); end destructor; "
    "end Widget; "
    "choice Outcome Fetched(owned w: Widget), Failed(status: int); "
    "end Outcome; "
    "class Client "
    "  public constructor() end constructor; "
    "  public method Fetch(): Outcome => Outcome.Fetched(new Widget()); "
    "end Client; ";

// A counting shell for the loop-amplified oracle.
const char* kCountingFetchDecls =
    "class Widget "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Widget._n := Widget._n + 1; end destructor; "
    "end Widget; "
    "choice Outcome Fetched(owned w: Widget), Failed(status: int); "
    "end Outcome; "
    "class Client "
    "  public constructor() end constructor; "
    "  public method Fetch(): Outcome => Outcome.Fetched(new Widget()); "
    "end Client; ";

std::string runOut(const std::string& decls, const std::string& body,
                   const std::string& extraMembers = "") {
    auto r = runAot(std::string("module M; ") + decls +
                    " class Host public static method Main() " + body +
                    " end Main; " + extraMembers + " end Host;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// --- §35.5: a DISCARDED owned payload is disposed at construct completion ---

BEZEL_TEST(codegen_owning_choice_case_scrutinee_temp_4552,
           spec_35_5_discarded_owned_payload_disposed_at_construct_completion) {
    // Rule-corpus case s35_5_0006 (§35.5, v0.306). `Fetched(_)` binds nothing,
    // so the payload is never transferred and is disposed when the construct
    // completes — BEFORE "after" runs, which is what makes the boundary
    // observable. Pre-fix observable was "hit\nafter".
    BEZEL_EXPECT_EQ(
        runOut(kFetchDecls,
               "using var client := new Client(); "
               "case client.Fetch() of "
               "  Fetched(_) => Console.WriteLine(\"hit\"), "
               "  Failed(_)  => Console.WriteLine(\"other\") "
               "end case; "
               "Console.WriteLine(\"after\");"),
        std::string("hit\nwidget disposed\nafter\n"));
}

BEZEL_TEST(codegen_owning_choice_case_scrutinee_temp_4552,
           spec_35_5_catch_all_arm_over_a_temporary_disposes_the_payloads) {
    // The other half of §35.5's sentence: "the payloads of the value when a
    // `_` or non-destructuring arm is selected" are disposed at completion
    // too. A bare `_` arm destructures nothing, so nothing was transferred.
    BEZEL_EXPECT_EQ(
        runOut(kFetchDecls,
               "using var client := new Client(); "
               "case client.Fetch() of "
               "  _ => Console.WriteLine(\"any\") "
               "end case; "
               "Console.WriteLine(\"after\");"),
        std::string("any\nwidget disposed\nafter\n"));
}

BEZEL_TEST(codegen_owning_choice_case_scrutinee_temp_4552,
           spec_35_5_exception_out_of_an_arm_disposes_during_unwinding) {
    // §35.5: "An exception out of an arm disposes the not-yet-consumed
    // remainder during unwinding (K3.15)." The disposal must therefore be
    // established at the scrutinee's production, not only emitted on the
    // normal edge — so it precedes the handler.
    BEZEL_EXPECT_EQ(
        runOut(kFetchDecls,
               "using var client := new Client(); "
               "try "
               "  case client.Fetch() of "
               "    Fetched(_) => throw new InvalidOperationException(\"boom\"), "
               "    Failed(_)  => Console.WriteLine(\"other\") "
               "  end case; "
               "catch "
               "  e: InvalidOperationException => Console.WriteLine(\"caught\") "
               "end try; "
               "Console.WriteLine(\"after\");"),
        std::string("widget disposed\ncaught\nafter\n"));
}

// --- The narrowing: a construct whose arms BIND must not be touched ---------

BEZEL_TEST(codegen_owning_choice_case_scrutinee_temp_4552,
           spec_35_5_arm_that_adopts_the_payload_still_disposes_exactly_once) {
    // Rule-corpus case s35_5_0002 (§35.5, v0.306) — §35.5's own adoption
    // spelling: "falling off the arm still owning the value auto-drops it …
    // the standard remedies (`using var u := move r;` adoption in a
    // `do … end` arm …)". The arm's `using var` is the owner here, so the
    // construct-completion reclaim must DECLINE. Without the arms-bind gate
    // this row is a double free (measured SIGSEGV), which is why the gate
    // exists and why this row sits beside the fix it constrains.
    BEZEL_EXPECT_EQ(
        runOut(kFetchDecls,
               "using var client := new Client(); "
               "case client.Fetch() of "
               "  Fetched(var w) => do "
               "    using var widget := move w; "
               "    widget.Ping(); "
               "  end, "
               "  Failed(_) => Console.WriteLine(\"failed\") "
               "end case;"),
        std::string("ping\nwidget disposed\n"));
}

BEZEL_TEST(codegen_owning_choice_case_scrutinee_temp_4552,
           spec_35_5_named_local_scrutinee_is_not_disposed_by_the_construct) {
    // A NAMED local is not an unbound temporary: §35.5's borrowing match reads
    // it, and §35.4's scope-end active-case pass is its owner. Disposing it at
    // the construct's completion would double-free at scope end. The disposal
    // therefore lands AFTER "after", not before it.
    BEZEL_EXPECT_EQ(
        runOut(kFetchDecls,
               "using var client := new Client(); "
               "var o := client.Fetch(); "
               "case o of "
               "  Fetched(_) => Console.WriteLine(\"hit\"), "
               "  Failed(_)  => Console.WriteLine(\"other\") "
               "end case; "
               "Console.WriteLine(\"after\");"),
        std::string("hit\nafter\nwidget disposed\n"));
}

// --- The loop-amplified leak oracle -----------------------------------------

BEZEL_TEST(codegen_owning_choice_case_scrutinee_temp_4552,
           spec_35_5_every_discarded_payload_in_a_loop_is_disposed) {
    // A one-shot disposal row is a weak oracle for a leak fix: amplify to 500
    // constructs, each producing and discarding one payload. Pre-fix the count
    // was 0.
    BEZEL_EXPECT_EQ(
        runOut(kCountingFetchDecls,
               "Host.Churn(); Console.WriteLine(Widget._n);",
               "public static method Churn() "
               "  using var client := new Client(); "
               "  var i := 0; "
               "  while i < 500 do "
               "    case client.Fetch() of "
               "      Fetched(_) => (), "
               "      Failed(_)  => () "
               "    end case; "
               "    i := i + 1; "
               "  end while; "
               "end Churn;"),
        std::string("500\n"));
}
