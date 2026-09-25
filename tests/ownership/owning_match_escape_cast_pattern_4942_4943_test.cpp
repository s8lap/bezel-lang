// K8.4 at the §35.5 owning-match boundary — two SOURCE-PRESERVING channels the
// #4818 fan-out (`collectOwningMatchSourceRoots`) did not follow:
//
//   #4942 — `x as T` and `x!` (spec §28.3 producer table: `x as T` is
//           "Borrowed (source: same as `x`'s source …)"; §17.8: "if `x` is
//           borrowed, `x!` is borrowed with the same source"). So
//           `(w as Widget)!` and `(w as Widget) ?? other` carry the
//           transferred payload binding `w` exactly as a bare `w` arm does.
//
// (spec §35.5 *The owning match*, §28.3 K8.3/K8.4 and K7.2/K2.2, §17.4,
// §17.8, v0.307)
//
// THE BUG. The source-root peel stopped at an `as` (TypeTestExpr) and at a
// postfix `!` (PostfixBangExpr), so every REJECT row below rooted at nothing
// and compiled clean. MEASURED on f3899b50 + eb2e1b72 (#4818): every REJECT
// row compiled and ran, printing `r=a` / `keep=a` — the escaped borrow
// reading payload `a` after the construct, a leak today and a
// heap-use-after-free under #4634's arm-end auto-drop.
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN compile, so each REJECT row
// asserts the SPECIFIC K8.4 text AND `!r.ran` (a one-shot leak oracle is
// vacuous here, #4715). Each ACCEPT row asserts an error-free compile, `ran`,
// and the exact stdout.
//
// per §35.5, §28.3 K8.4, §17.4, §17.8, v0.307

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
