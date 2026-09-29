// Codegen regression tests for issue #4552 (the fourth of its four OMITTED
// disposals): §28.3 *Unbound owned temporaries*, spec v0.306.
//
// §28.3: "An owned value may also be produced as an **unbound temporary**: the
// result of an `owned`-returning call (or other owned producer) that is not
// bound to any name, not `move`d, and not returned, but used only by
// *borrowing* operations within the expression — as the receiver of a further
// call, as a non-`owned` argument, as an operator operand, as a member or
// index access … Such a temporary is disposed at the **end of the full
// expression** in which it is produced, after every use of it within that
// expression has completed."
//
// THE BUG. The fresh-receiver reclaim (`freshNewReceiverTempEligible`) fires
// only when the call's result is provably free of any reference into the
// receiver — a numeric/bool/char/decimal primitive, an enum, unit, or an
// `owned`-announced class/collection. `string` is excluded there with the
// comment "a field / method result borrows the receiver's refcounted string",
// so `new Widget("temp").Describe()` disposed nothing: rule-corpus case
// s28_3_0043 wanted `widget temp / dispose temp` and observed `widget temp`.
//
// WHY THE EXCLUSION IS UNNECESSARY FOR A NON-NULLABLE `string`, which is the
// whole soundness argument: a non-nullable `string` return is minted `+1` BY
// THE CALLEE. The return path emits `bz_string_retain` for any returned value
// that is not already a producer (LlvmBackend_Stmts.cpp's return arm and its
// expression-body twin, both gated `!nullable`), so even the identity shape
// `Label(): string => _label` hands back its own reference and the receiver's
// disposal — which releases the receiver's string fields — cannot dangle it.
// A `string?` return does NOT mint (#2196 deliberately left that path
// un-refcounted), so it stays excluded and keeps leaking.
//
// WHY THE RECLAIM IS DEFERRED rather than emitted inline after the call, which
// is what the rest of the family does: §28.3 puts it at the end of the full
// expression, "after every use of it within that expression has completed",
// and the receiver's destructor is user code. Inline, s28_3_0043 would print
// `dispose temp` BEFORE `widget temp` — a different defect (#4552's own
// ordering group) wearing this one's clothes. So the rows below assert the
// ORDER, not merely that a disposal happened.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kWidgetDecls =
    "class Widget "
    "  private _label: string; "
    "  public constructor(label: string) _label := label; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_label}\"); end destructor; "
    "  public method Describe(): string => $\"widget {_label}\"; "
    "  public method Label(): string => _label; "
    "  public method Tag(): string? => null; "
    "end Widget; ";

std::string runOut(const std::string& decls, const std::string& body,
                   const std::string& extraMembers = "") {
    auto r = runAot(std::string("module M; ") + decls +
                    " class Host public static method Main() " + body +
                    " end Main; " + extraMembers + " end Host;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// --- §28.3: the temporary is disposed at the END of the full expression -----

BEZEL_TEST(codegen_unbound_owned_receiver_string_result_4552,
           spec_28_3_receiver_temp_with_string_result_disposed_at_expr_end) {
    // Rule-corpus case s28_3_0043 (§28.3, v0.306), verbatim. The boundary is
    // observable because both the callee's result and the destructor print:
    // `widget temp` must come FIRST. Pre-fix observable was `widget temp`
    // alone.
    BEZEL_EXPECT_EQ(
        runOut(kWidgetDecls,
               "Console.WriteLine(new Widget(\"temp\").Describe());"),
        std::string("widget temp\ndispose temp\n"));
}

BEZEL_TEST(codegen_unbound_owned_receiver_string_result_4552,
           spec_28_3_receiver_temp_disposed_at_end_of_an_initializer) {
    // §28.3's boundary is the full expression, so in a declaration it is the
    // INITIALIZER's end — before the next statement runs, not at scope end.
    BEZEL_EXPECT_EQ(
        runOut(kWidgetDecls,
               "var s := new Widget(\"bound\").Describe(); "
               "Console.WriteLine(\"between\"); "
               "Console.WriteLine(s);"),
        std::string("dispose bound\nbetween\nwidget bound\n"));
}

BEZEL_TEST(codegen_unbound_owned_receiver_string_result_4552,
           spec_28_3_string_result_survives_the_receivers_disposal) {
    // The soundness property in executable form: `Label()` returns the
    // receiver's own string FIELD, and disposing the receiver releases that
    // field's reference. The result nonetheless reads correctly afterwards,
    // because a non-nullable `string` return is minted `+1` by the callee.
    // If that convention ever changes, this row is the tripwire.
    BEZEL_EXPECT_EQ(
        runOut(kWidgetDecls,
               "var s := new Widget(\"field\").Label(); "
               "Console.WriteLine(s);"),
        std::string("dispose field\nfield\n"));
}

BEZEL_TEST(codegen_unbound_owned_receiver_string_result_4552,
           spec_28_3_nullable_string_result_keeps_the_conservative_leak) {
    // The exclusion that STAYS: a `string?` return is not minted, so the
    // result can be a borrow of a receiver field and the receiver may not be
    // disposed here. Pinning the declined half keeps a future widening from
    // quietly taking the unsound direction — "dispose tag" must NOT appear.
    BEZEL_EXPECT_EQ(
        runOut(kWidgetDecls,
               "var t := new Widget(\"tag\").Tag(); "
               "Console.WriteLine(\"done\");"),
        std::string("done\n"));
}

// --- The loop-amplified leak oracle -----------------------------------------

BEZEL_TEST(codegen_unbound_owned_receiver_string_result_4552,
           spec_28_3_every_receiver_temp_in_a_loop_is_disposed) {
    // A one-shot disposal row is a weak oracle for a leak fix: 500 iterations
    // produce 500 receiver temporaries. Pre-fix the count was 0.
    BEZEL_EXPECT_EQ(
        runOut("class Widget "
               "  public static _n: int := 0; "
               "  public constructor() end constructor; "
               "  destructor Widget._n := Widget._n + 1; end destructor; "
               "  public method Describe(): string => \"w\"; "
               "end Widget; ",
               "var i := 0; "
               "while i < 500 do "
               "  var s := new Widget().Describe(); "
               "  i := i + 1; "
               "end while; "
               "Console.WriteLine(Widget._n);"),
        std::string("500\n"));
}

// --- The exception edge ------------------------------------------------------

BEZEL_TEST(codegen_unbound_owned_receiver_string_result_4552,
           spec_28_3_receiver_temp_disposed_when_the_expression_throws) {
    // §28.3: "When the full expression is instead exited by an **exception** …
    // every already-produced, not-yet-consumed unbound owned temporary is
    // disposed during unwinding by the same implicit `try`/`finally`
    // mechanism as named locals … each temporary's disposal is established at
    // its production". The registration is at production, before the call, so
    // a throw from the callee still reclaims the receiver — before the handler
    // runs.
    BEZEL_EXPECT_EQ(
        runOut("class Widget "
               "  private _label: string; "
               "  public constructor(label: string) _label := label; "
               "  end constructor; "
               "  destructor Console.WriteLine($\"dispose {_label}\"); "
               "  end destructor; "
               "  public method Boom(): string => "
               "    throw new InvalidOperationException(\"boom\"); "
               "end Widget; ",
               "try "
               "  Console.WriteLine(new Widget(\"throwing\").Boom()); "
               "catch "
               "  e: InvalidOperationException => Console.WriteLine(\"caught\") "
               "end try;"),
        std::string("dispose throwing\ncaught\n"));
}
