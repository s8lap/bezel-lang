// K8.4 at the §35.5 owning-match boundary — the VALUE-SELECTING channel: an
// arm result or an outer store whose value is an `if … then … else …`
// expression, a nested `case … of` expression, or a `??`, with the transferred
// payload binding among its selected values (issue #4818; spec §35.5 *The
// owning match*, §28.3 K8.3/K8.4 and K7.2/K2.2, v0.307).
//
// THE RULE. K8.3 puts the scrutinee's boundary at the whole `case` construct;
// K8.4 rejects a borrow that would escape it, "never lifetime-extended". K7
// names `??`, `if` expressions and expression-position `case` arms as joins,
// and K7.2 gives a borrowed ⊓ borrowed join "the union of the arms' source
// sets (K2.2)". So `(if p then w else other)` carries `w` as a source exactly
// as `w` does, and leaving the arm with it is the same K8.4 escape.
//
// THE BUG (#4818). #4635 / #4663 / #4715 / #4720 resolved the escaping value
// with `identifierRoot`, which follows member access, calls, indexers and the
// binding atom but stops dead at a value-selecting form — so every row below
// rooted at nothing and compiled clean. MEASURED on 7ca1f17c: all eleven
// REJECT rows compiled and ran, printing `r=a` / `keep=a` — the escaped
// borrow reading payload `a`, which leaks today and is a heap-use-after-free
// once #4634's arm-end auto-drop lands (the issue measured that with
// cc14f07e).
//
// THE FIX. `collectOwningMatchSourceRoots` fans out at the three selecting
// forms (both values of a conditional, every expression arm of a `case`, both
// operands of `??`) — never into a condition, scrutinee or guard, which are
// only read — and `owningMatchEscapeRoot` traces every root it returns. The
// capture route's lambda search follows the same three forms.
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN compile, so each REJECT row
// asserts the SPECIFIC K8.4 text AND `!r.ran`. A leak oracle would be vacuous
// (#4715 measured one-shot rows printing no LSan report on either side). Each
// ACCEPT row asserts an error-free compile, `ran`, and the exact stdout, so an
// over-rejection or a wrong selected value both fail it.
//
// per §35.5, §28.3 K7.2/K8.4, v0.307

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

// The issue's own prelude (review-4634/{p,q}): `Widget` owns an `Inner`, so
// `w.In()` is a DERIVED borrow of the payload.
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
    "      Outer.W(new Widget(\"a\"), new Widget(\"b\")); ";

std::string prog(const std::string& mainBody) {
    return std::string(kPrelude) +
           "  public static method Main(): int " + mainBody +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// Two outer owners the selected value may legitimately be, plus the
// owning-choice scrutinee. `Widget`-typed and `Inner`-typed variants.
const char* kWidgets =
    "    using var other := new Widget(\"other\"); "
    "    using var other2 := new Widget(\"other2\"); "
    "    var p := true; "
    "    var o := Mk(); ";
const char* kInners =
    "    using var other := new Inner(\"other\"); "
    "    using var other2 := new Inner(\"other2\"); "
    "    var p := true; "
    "    var o := Mk(); ";

// Expression position: `var r := (case move o of W(var w, _) => ARM, X =>
// other);` then read `r` after the construct.
std::string exprArm(const char* setup, const std::string& arm,
                    const std::string& xArm = "other") {
    return prog(std::string(setup) +
                "    var r := (case move o of W(var w, _) => " + arm +
                ", X => " + xArm + "); "
                "    Console.WriteLine($\"r={r.Name()}\"); ");
}

// Statement position: store into the pre-existing outer local `keep`, then
// read it after the construct.
std::string stmtArm(const char* setup, const std::string& armBody) {
    return prog(std::string(setup) +
                "    var keep := other; "
                "    case move o of "
                "      W(var w, _) => do " + armBody + " end, "
                "      X => () "
                "    end case; "
                "    Console.WriteLine($\"keep={keep.Name()}\"); ");
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

}  // namespace

// ===========================================================================
// REJECT rows — every one measured compiling clean and printing the payload's
// name on 7ca1f17c.
// ===========================================================================

// (1) THE ISSUE'S REPRO (a7_nested_expr_result_w), verbatim shape: a
// conditional arm result selecting `w`, with `w` also read in the condition.
BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_conditional_selects_w_rejected_28_3_k8_4) {
    expectRejected(
        exprArm(kWidgets, "(if w.Ping() = 7 then w else other)"));
}

// (2) c1_stmt_assign_cond_w: the same conditional stored into an outer local
// from a statement-position arm.
BEZEL_TEST(owning_match_value_select_escape_4818,
           stmt_store_conditional_selects_w_rejected_28_3_k8_4) {
    expectRejected(
        stmtArm(kWidgets, "keep := (if w.Ping() = 7 then w else other);"));
}

// (3) c2_expr_nested_case_w: a nested (non-owning) `case` expression whose
// arm result is `w`.
BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_nested_case_selects_w_rejected_28_3_k8_4) {
    expectRejected(exprArm(kWidgets, "(case 1 of 1 => w, _ => other)"));
}

// (4) c3_expr_cond_derived: a DERIVED borrow under the conditional.
BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_conditional_selects_derived_borrow_rejected_28_3_k8_4) {
    expectRejected(
        exprArm(kInners, "(if true then w.In() else other)"));
}

// (5) c5_expr_coalesce: `??` with `w` reached through its left operand.
BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_coalesce_selects_w_rejected_28_3_k8_4) {
    expectRejected(
        exprArm(kWidgets, "(if false then null else w) ?? other"));
}

// (6) An elif chain: `w` in the right-nested ConditionalExpr (§20/A.7).
BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_elif_chain_selects_w_rejected_28_3_k8_4) {
    expectRejected(exprArm(
        kWidgets,
        "(if not p then other elif w.Ping() = 7 then w else other2)"));
}

// (7) The derived borrow OVER the selecting form rather than under it:
// `(if p then w else other).In()`.
BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_member_over_conditional_rejected_28_3_k8_4) {
    expectRejected(exprArm(kWidgets, "(if p then w else other).In()",
                           "other.In()"));
}

// (8)/(9) The statement-position forms of the nested-case and `??` rows.
BEZEL_TEST(owning_match_value_select_escape_4818,
           stmt_store_nested_case_selects_w_rejected_28_3_k8_4) {
    expectRejected(
        stmtArm(kWidgets, "keep := (case 1 of 1 => w, _ => other);"));
}

BEZEL_TEST(owning_match_value_select_escape_4818,
           stmt_store_coalesce_selects_w_rejected_28_3_k8_4) {
    expectRejected(
        stmtArm(kWidgets, "keep := (if false then null else w) ?? other;"));
}

// (10) Statement-position derived borrow under the conditional.
BEZEL_TEST(owning_match_value_select_escape_4818,
           stmt_store_conditional_derived_borrow_rejected_28_3_k8_4) {
    expectRejected(stmtArm(kInners, "keep := (if p then w.In() else other);"));
}

// (11) The conditional behind a same-arm `const` alias (#4715's trace): the
// alias's INITIALIZER is the selecting form.
BEZEL_TEST(owning_match_value_select_escape_4818,
           stmt_store_via_alias_of_conditional_rejected_28_3_k8_4) {
    expectRejected(stmtArm(
        kWidgets, "const c := (if p then w else other); keep := c;"));
}

// (12) The capture route (#4663) through a nested `case`: the selected value
// is a closure capturing `w`.
BEZEL_TEST(owning_match_value_select_escape_4818,
           stmt_store_nested_case_selects_capturing_closure_rejected_28_3_k8_4) {
    JitResult r = runAot(prog(
        "    var o := Mk(); "
        "    var f: () -> int := lambda(): int => 0; "
        "    case move o of "
        "      W(var w, _) => do f := (case 1 of 1 => lambda(): int => "
        "          w.Ping(), _ => lambda(): int => 1); end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"f={f()}\"); "));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "captures owning-match payload binding `w`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// ACCEPT controls — the selecting form's SELECTED values are not derived from
// the payload; `w` is only READ (in a condition or a scrutinee). K7.2 unions
// the selected values' sources only, so these must stay accepted, and must
// print the value actually selected.
// ===========================================================================

BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_conditional_reads_w_only_in_condition_accepted) {
    expectAccepted(
        exprArm(kWidgets, "(if w.Ping() = 7 then other else other2)"),
        "dispose b\ndispose inner b\nr=other\n"
        "dispose other2\ndispose inner other2\n"
        "dispose other\ndispose inner other\n");
}

BEZEL_TEST(owning_match_value_select_escape_4818,
           stmt_store_conditional_reads_w_only_in_condition_accepted) {
    expectAccepted(
        stmtArm(kWidgets, "keep := (if w.Ping() = 7 then other2 else other);"),
        "dispose b\ndispose inner b\nkeep=other2\n"
        "dispose other2\ndispose inner other2\n"
        "dispose other\ndispose inner other\n");
}

BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_nested_case_scrutinizes_w_only_accepted) {
    expectAccepted(
        exprArm(kWidgets, "(case w.Ping() of 7 => other2, _ => other)"),
        "dispose b\ndispose inner b\nr=other2\n"
        "dispose other2\ndispose inner other2\n"
        "dispose other\ndispose inner other\n");
}

BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_coalesce_of_outer_owners_accepted) {
    expectAccepted(
        exprArm(kWidgets,
                "(if w.Ping() = 7 then null else other) ?? other2"),
        "dispose b\ndispose inner b\nr=other2\n"
        "dispose other2\ndispose inner other2\n"
        "dispose other\ndispose inner other\n");
}

// A VALUE-typed selection (`int`) is a copy, never a borrow — the K8.4 gate
// on borrow-carrying types must still admit it with `w` in a selected value.
BEZEL_TEST(owning_match_value_select_escape_4818,
           expr_arm_conditional_value_result_accepted) {
    expectAccepted(
        prog(std::string(kWidgets) +
             "    var n := (case move o of W(var w, _) => "
             "        (if p then w.Ping() else 0), X => 0); "
             "    Console.WriteLine($\"n={n}\"); "),
        "dispose b\ndispose inner b\nn=7\n"
        "dispose other2\ndispose inner other2\n"
        "dispose other\ndispose inner other\n");
}

// A derived READ of the payload in the condition (`w.In().Ping()`), with the
// selected values both outer owners.
BEZEL_TEST(owning_match_value_select_escape_4818,
           stmt_store_conditional_derived_read_in_condition_accepted) {
    expectAccepted(
        stmtArm(kInners,
                "keep := (if w.In().Ping() = 11 then other2 else other);"),
        "dispose b\ndispose inner b\nkeep=other2\n"
        "dispose inner other2\ndispose inner other\n");
}
