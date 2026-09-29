// K8.4 at the §35.5 owning-match boundary — two SOURCE-PRESERVING channels the
// #4818 fan-out (`collectOwningMatchSourceRoots`) did not follow:
//
//   #4942 — `x as T` and `x!` (spec §28.3 producer table: `x as T` is
//           "Borrowed (source: same as `x`'s source …)"; §17.8: "if `x` is
//           borrowed, `x!` is borrowed with the same source"). So
//           `(w as Widget)!` and `(w as Widget) ?? other` carry the
//           transferred payload binding `w` exactly as a bare `w` arm does.
//
//   #4943 — an inner pattern binding (spec §19.3: a pattern binding "is a
//           borrow whose source is the scrutinee's source"). Inside the arm,
//           the `q` of a nested borrowing `case w of var q: Widget => …`
//           carries `w`, in expression and statement position alike.
//
// (spec §35.5 *The owning match*, §28.3 K8.3/K8.4 and K7.2/K2.2, §17.4,
// §17.8, v0.307)
//
// THE BUGS. #4942: the source-root peel stopped at an `as` (TypeTestExpr) and
// at a postfix `!` (PostfixBangExpr). #4943: the peel reached `q` but `q` is
// neither a transferred binding nor an arm-local alias, so the trace stopped
// there. Either way every REJECT row below rooted at nothing and compiled
// clean. MEASURED on f3899b50 + eb2e1b72 (#4818): every REJECT
// row compiled and ran, printing `r=a` / `keep=a` — the escaped borrow
// reading payload `a` after the construct, a leak today and a
// heap-use-after-free under #4634's arm-end auto-drop.
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN compile, so each REJECT row
// asserts the SPECIFIC K8.4 text AND `!r.ran` (a one-shot leak oracle is
// vacuous here, #4715). Each ACCEPT row asserts an error-free compile, `ran`,
// and the exact stdout.
//
// per §35.5, §28.3 K8.4, §17.4, §17.8, §19.3, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The K8.4 escape diagnostic's shared text, as both #4635's arm-result form
// and #4663's store form spell it, naming the binding.
const char* kEscapeNeedle =
    "owning-match payload binding `w`, whose payload does not outlive the "
    "`case` construct";

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int errorCount(const JitResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

// The #4818 test prelude: `Widget` owns an `Inner`, so `w.In()` is a DERIVED
// borrow of the payload.
const char* kPrelude =
    "module M; "
    "public class Inner "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose inner {_n}\"); end destructor; "
    "  public method Ping(): int => 11; "
    "  public method Name(): string => _n; "
    "end Inner; "
    "public class Widget "
    "  private _n: string; "
    "  private owned _in: Inner; "
    "  public constructor(n: string) _n := n; _in := new Inner(n); "
    "  end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "  public method Name(): string => _n; "
    "  public method In(): Inner => _in; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public class Program "
    "  public static method Mk(): Outer => "
    "      Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
    "  public static method Main(): int "
    "    using var other := new Widget(\"other\"); "
    "    using var other2 := new Widget(\"other2\"); "
    "    var p := true; "
    "    var o := Mk(); ";

const char* kEpilogue =
    "    return 0; "
    "  end Main; "
    "end Program;";

// Expression position: `var r := (case move o of W(var w, _) => ARM, X =>
// XARM);` then read `r` after the construct.
std::string exprArm(const std::string& arm,
                    const std::string& xArm = "other") {
    return std::string(kPrelude) +
           "    var r := (case move o of W(var w, _) => " + arm +
           ", X => " + xArm + "); "
           "    Console.WriteLine($\"r={r.Name()}\"); " + kEpilogue;
}

// Statement position: store into the pre-existing outer local `keep`, then
// read it after the construct.
std::string stmtArm(const std::string& armBody) {
    return std::string(kPrelude) +
           "    var keep := other; "
           "    case move o of "
           "      W(var w, _) => do " + armBody + " end, "
           "      X => () "
           "    end case; "
           "    Console.WriteLine($\"keep={keep.Name()}\"); " + kEpilogue;
}

void expectRejected(const std::string& program) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kEscapeNeedle));
    BEZEL_EXPECT_FALSE(r.ran);
}

void expectAccepted(const std::string& program, const std::string& out) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// Payload `b` (the un-transferred remainder) is disposed at construct
// completion; the two outer owners at `Main`'s end, in reverse order.
const char* kTailOther2 =
    "dispose other2\ndispose inner other2\n"
    "dispose other\ndispose inner other\n";

std::string acceptedOut(const std::string& line) {
    return "dispose b\ndispose inner b\n" + line + "\n" + kTailOther2;
}

}  // namespace

// ===========================================================================
// #4942 REJECT rows — `as` / `!` over the transferred binding. Every one
// MEASURED compiling clean and printing the payload's name on f3899b50 +
// eb2e1b72.
// ===========================================================================

// The issue's C1: `(w as Widget)!` as the arm's value.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_cast_forgive_w_rejected_4942_28_3_k8_4) {
    expectRejected(exprArm("(w as Widget)!"));
}

// The issue's C2: `(w as Widget) ?? other` — `??` fans out (#4818), and its
// left operand is the cast.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_cast_coalesce_w_rejected_4942_28_3_k8_4) {
    expectRejected(exprArm("(w as Widget) ?? other"));
}

// The issue's B4: the cast under a conditional under a `??`.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_cast_in_conditional_coalesce_rejected_4942_28_3_k8_4) {
    expectRejected(exprArm("(if p then w as Widget else other) ?? other2"));
}

// The issue's B5: the forgiven cast as a conditional's selected value.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_forgive_in_conditional_rejected_4942_28_3_k8_4) {
    expectRejected(exprArm("(if p then (w as Widget)! else other)"));
}

// A DERIVED borrow over the forgiven cast: `(w as Widget)!.In()`.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_cast_forgive_derived_member_rejected_4942_28_3_k8_4) {
    expectRejected(exprArm("(w as Widget)!.In()", "other.In()"));
}

// Statement position: the forgiven cast and the coalesced cast stored into an
// outer local (#4663's ordered store walk).
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_store_cast_forgive_w_rejected_4942_28_3_k8_4) {
    expectRejected(stmtArm("keep := (w as Widget)!;"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_store_cast_coalesce_w_rejected_4942_28_3_k8_4) {
    expectRejected(stmtArm("keep := (w as Widget) ?? other;"));
}

// The cast behind a same-arm `const` alias (#4715's trace).
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_store_via_alias_of_cast_rejected_4942_28_3_k8_4) {
    expectRejected(stmtArm("const c := (w as Widget)!; keep := c;"));
}

// ===========================================================================
// #4942 ACCEPT controls — `as` / `!` over an UNRELATED outer owner, and a cast
// of `w` that is only READ in a condition. Must stay accepted and print the
// value actually selected.
// ===========================================================================

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_cast_forgive_unrelated_owner_accepted_4942) {
    expectAccepted(exprArm("(other2 as Widget)!"), acceptedOut("r=other2"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_cast_coalesce_unrelated_owner_accepted_4942) {
    expectAccepted(exprArm("(other2 as Widget) ?? other"),
                   acceptedOut("r=other2"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_store_cast_forgive_unrelated_owner_accepted_4942) {
    expectAccepted(stmtArm("keep := (other2 as Widget)!;"),
                   acceptedOut("keep=other2"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_cast_of_w_read_only_in_condition_accepted_4942) {
    expectAccepted(
        exprArm("(if (w as Widget)!.Ping() = 7 then other2 else other)"),
        acceptedOut("r=other2"));
}

// ===========================================================================
// #4943 REJECT rows — the inner `case`'s pattern binding `q` roots at its
// scrutinee `w`. Every one MEASURED compiling clean and printing the
// payload's name (`r=a` / `keep=a` / `f=7`) on f3899b50 + eb2e1b72.
// ===========================================================================

// The issue's B1: expression position, `q` as the inner arm's value.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_inner_case_binding_selected_rejected_4943_19_3_k8_4) {
    expectRejected(exprArm("(case w of var q: Widget => q, _ => other)"));
}

// The issue's C4: the same inner case stored into an outer local.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_store_inner_case_binding_selected_rejected_4943_19_3_k8_4) {
    expectRejected(
        stmtArm("keep := (case w of var q: Widget => q, _ => other);"));
}

// A DERIVED borrow of the binding: `q.In()`.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_inner_case_binding_member_rejected_4943_19_3_k8_4) {
    expectRejected(exprArm("(case w of var q: Widget => q.In(), _ => other.In())",
                           "other.In()"));
}

// A DERIVED scrutinee: `case w.In() of var q: Inner => q`.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_inner_case_over_derived_scrutinee_rejected_4943_19_3_k8_4) {
    expectRejected(exprArm(
        "(case w.In() of var q: Inner => q, _ => other.In())", "other.In()"));
}

// A cast scrutinee: `case w as Widget of var q: Widget => q`, which needs the
// #4942 peel on the scrutinee.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_inner_case_over_cast_scrutinee_rejected_4943_19_3_k8_4) {
    expectRejected(
        exprArm("(case w as Widget of var q: Widget => q, _ => other)"));
}

// The inner case behind a same-arm `const` alias (#4715's trace).
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_store_via_alias_of_inner_case_rejected_4943_19_3_k8_4) {
    expectRejected(stmtArm(
        "const c := (case w of var q: Widget => q, _ => other); keep := c;"));
}

// A nested STATEMENT `case` whose block arm stores `q` to the outer local.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_inner_case_stmt_stores_binding_rejected_4943_19_3_k8_4) {
    expectRejected(stmtArm(
        "case w of var q: Widget => do keep := q; end, _ => () end case;"));
}

// The same store under a conditional inside the inner arm.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_inner_case_stmt_conditional_store_rejected_4943_19_3_k8_4) {
    expectRejected(stmtArm(
        "case w of var q: Widget => do if p then keep := q; end; end, "
        "_ => () end case;"));
}

// A kill of `q` on ONE path only: on the `p` path `q` is still `w` when it is
// stored, so the taint must survive the merge (#4720's union rule).
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_inner_case_stmt_partial_kill_rejected_4943_19_3_k8_4) {
    expectRejected(stmtArm(
        "case w of var q: Widget => do if not p then q := other2; end; "
        "keep := q; end, _ => () end case;"));
}

// The capture route (#4663): a closure capturing `q`, selected by the inner
// case, stored into an outer local.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_store_inner_case_closure_captures_binding_rejected_4943) {
    JitResult r = runAot(
        std::string(kPrelude) +
        "    var f: () -> int := lambda(): int => 0; "
        "    case move o of "
        "      W(var w, _) => do f := (case w of var q: Widget => "
        "          lambda(): int => q.Ping(), _ => lambda(): int => 1); end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"f={f()}\"); " + kEpilogue);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "captures owning-match payload binding `w`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// #4943 ACCEPT controls — an inner case whose SELECTED values are only outer
// owners (the binding is read, or unused), an inner case over an outer owner,
// and a statement arm that kills `q`'s taint before storing it.
// ===========================================================================

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_inner_case_selects_only_outer_owners_accepted_4943) {
    expectAccepted(
        exprArm("(case w of var q: Widget => "
                "(if q.Ping() = 7 then other2 else other), _ => other)"),
        acceptedOut("r=other2"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_inner_case_binding_unused_accepted_4943) {
    expectAccepted(exprArm("(case w of var q: Widget => other2, _ => other)"),
                   acceptedOut("r=other2"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_inner_case_binding_read_only_in_guard_accepted_4943) {
    expectAccepted(
        exprArm("(case w of var q: Widget when q.Ping() = 7 => other2, "
                "_ => other)"),
        acceptedOut("r=other2"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           expr_arm_inner_case_over_outer_owner_accepted_4943) {
    expectAccepted(
        exprArm("(case other2 of var q: Widget => q, _ => other)"),
        acceptedOut("r=other2"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_inner_case_stmt_over_outer_owner_accepted_4943) {
    expectAccepted(
        stmtArm("case other2 of var q: Widget => do keep := q; end, "
                "_ => () end case;"),
        acceptedOut("keep=other2"));
}

BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_inner_case_stmt_reads_binding_only_accepted_4943) {
    expectAccepted(
        stmtArm("case w of var q: Widget => do "
                "Console.WriteLine($\"q={q.Name()}\"); end, _ => () end case; "
                "keep := other2;"),
        "q=a\n" + acceptedOut("keep=other2"));
}

// `q := other2;` kills the taint on every path before the store, so the
// store is of `other2`. Without the binding in `localDeclsInArm` the kill
// would not register and this sound program would be rejected.
BEZEL_TEST(owning_match_escape_cast_pattern_4942_4943,
           stmt_inner_case_stmt_killed_binding_store_accepted_4943) {
    expectAccepted(
        stmtArm("case w of var q: Widget => do q := other2; keep := q; end, "
                "_ => () end case;"),
        acceptedOut("keep=other2"));
}
