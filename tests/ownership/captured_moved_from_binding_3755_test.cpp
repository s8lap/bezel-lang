// Spec-derived tests for issue #3755 — §28.3 "Captured owned bindings" /
// §27.5 "Closures" / §28.8.1 "The `move` expression" (v0.286).
//
// Normative text this file pins, quoted verbatim:
//
//   §28.3 (*Captured owned bindings*): "Inside a closure, a captured owned
//   binding — an owned local or `owned` parameter of the enclosing body — is
//   uniformly in **borrow state**: reads borrow (the producers rows above),
//   and assignment to it, a marked transfer from it, an explicit `Dispose()`
//   or discard of it, and every other owned-consuming use of it are compile
//   errors".
//
//   §27.5: "A lambda may reference variables from its enclosing scope.
//   Captured variables are heap-allocated in a closure object. … (For a
//   captured **owned** binding the sharing is read-only in the ownership
//   sense: the lambda may not assign to it, transfer from it, or dispose it
//   — §28.3.)"
//
//   §28.8.1: "The restoration machinery below is specific to fields and
//   slots: a moved-from *binding* is simply unreadable until reassigned, with
//   no restoration obligation."
//
// Composing the three: a capture is a READ of the binding (§28.3 classifies it
// as borrow-state, "reads borrow"; §27.5 says the closure object holds the
// binding), it happens where the closure literal is evaluated, and a
// moved-from binding is unreadable. Capturing a binding that is already
// moved-from at the literal is therefore the same error the checker already
// reports for a DIRECT read after the move ("owned value in `p` was already
// moved out at this point (§28.3)").
//
// The bug (#3755): no read check descended into a lambda literal's body, so
// the capture form was never checked. Measured on the parent (a31fb7b), with
// `bezelc a.bzl -o a.out`:
//
//     var q := move p;
//     this.h := lambda(): int => p.V();   // p is MovedFrom here
//
// compiled rc=0 with NO diagnostic and printed a garbage int (`-571446371`
// and `-1803718759` on two consecutive runs of the same binary). The `owned`
// PARAMETER form and the plain owned LOCAL form behave identically, so this is
// not the parameter/local axis fix #3746 closed. Neither is it #3746's
// store-escape channel: that check requires a LIVE owned source
// (`isOwnedTrackedLocal`), and a MovedFrom binding is not live, so it stays
// silent here — and the `var f := lambda(): int => p.V();` LocalDecl form
// performs no field store at all, so it is outside that channel entirely.
//
// Fail-without / pass-with: on the parent every NEG row below compiles clean
// (r.diagnostics.hasErrors() == false, and the AOT row's program RUNS and
// prints garbage); with the fix each is rejected with the §28.3 capture
// diagnostic. Every POS row compiles clean in BOTH directions — they are the
// false-reject guard, in particular the two SHADOWING rows, which are the
// shapes a name-keyed (rather than symbol-keyed) implementation would wrongly
// reject.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// `R` is an ordinary owned class with a destructor, so its disposal is a real
// event and a read after the move is a genuine read of freed storage. `Box`
// holds the function-typed field the issue's repro stores the closure into.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class Box "
        "  public h: (() -> int)?; "
        ) + body +
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// The rule-specific predicate (issue 2988's discipline): a NEG row must be
// load-bearing on THIS diagnostic, not on `hasErrors()`. A fail-without build
// accepts the program outright and produces no message at all.
bool rejectsMovedFromCapture(const OwnershipResult& r, const char* name) {
    return hasErrorMessage(r, std::string("closure captures `") + name +
                                  "` whose owned value was already moved out");
}

// --- The same-STATEMENT leg (review follow-up) ----------------------------
//
// `Go` consumes its `owned` parameter and THEN invokes the callback, which is
// what makes both argument orders unsafe: nothing at the call site orders the
// closure's invocation against the transfer. §22.5 *Evaluation order* fixes
// only when the argument EXPRESSIONS run ("A call's supplied argument
// expressions are evaluated in call-site textual order — left to right as
// written, for positional and named arguments alike").
std::string wrap2(const std::string& body) {
    return std::string(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class Holder "
        "  public n: int := 0; "
        "  public constructor(x: owned R, g: () -> int) "
        "    x.Dispose(); this.n := g(); "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Go(x: owned R, f: () -> int): int "
        "    x.Dispose(); return f(); "
        "  end Go; "
        "  public static method Keep(x: owned R): int "
        "    x.Dispose(); return 1; "
        "  end Keep; "
        "  public static method Run(): int ") + body +
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;";
}

// The same-statement leg's own diagnostic. Distinct wording from the
// moved-at-the-literal leg above, so a row cannot pass on the wrong one.
bool rejectsSameStatementTransfer(const OwnershipResult& r, const char* name) {
    return hasErrorMessage(r, std::string("closure captures `") + name +
                                  "` whose owned value is transferred away by "
                                  "this same statement");
}

// --- Expression-level MUTUALLY EXCLUSIVE branches -------------------------
//
// `Consume` takes the value away and hands back an unrelated closure;
// `ConsumeB` is its bool-returning form, for putting the consume in a
// CONDITION (which runs on every path). `Run(false)` selects the arm that does
// NOT consume, so the accept rows have a well-defined value to check.
std::string wrap3(const std::string& body) {
    return std::string(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class C "
        "  public static method Consume(x: owned R): () -> int "
        "    x.Dispose(); return lambda(): int => 99; "
        "  end Consume; "
        "  public static method ConsumeB(x: owned R): bool "
        "    x.Dispose(); return true; "
        "  end ConsumeB; "
        "  public static method Go(x: owned R, f: () -> int): int "
        "    x.Dispose(); return f(); "
        "  end Go; "
        "  public static method Run(flag: bool): int ") + body +
        "  end Run; "
        "  public static method Main(): int return Run(false); end Main; "
        "end C;";
}

} // namespace

// --- NEG: the binding is MovedFrom at the lambda literal (REJECT) ----------

// The issue's repro, `owned` PARAMETER form. `p` is moved into `q`, then
// captured. §28.3: the capture reads a moved-from binding.
BEZEL_TEST(own_captured_moved_from_3755, owned_param_moved_before_literal_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(p: owned R) "
        "  var q := move p; "
        "  this.h := lambda(): int => p.V(); "
        "  _ := move q; "
        "end Take; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// The issue's LOCAL twin — a plain (non-`using`) owned local, same move /
// capture / store sequence. Measured byte-for-byte identical on the parent
// (compiled rc=0, printed `1169353245`), which is what shows the defect is not
// on the parameter/local axis.
BEZEL_TEST(own_captured_moved_from_3755, owned_local_moved_before_literal_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take() "
        "  var p := new R(); "
        "  var q := move p; "
        "  this.h := lambda(): int => p.V(); "
        "  _ := move q; "
        "end Take; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// The LocalDecl-initializer form: the closure is bound to a local, never
// stored into a field. This shape is entirely outside fix #3746's store-escape
// channel (there is no field store to check), so it isolates the read rule.
BEZEL_TEST(own_captured_moved_from_3755, localdecl_initializer_form_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  var q := move p; "
        "  var f := lambda(): int => p.V(); "
        "  _ := f(); "
        "  _ := move q; "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// A BLOCK-bodied lambda: the capture lives in a statement of the body, not in
// an `exprBody`. Both shapes route through the same free-variable walk.
BEZEL_TEST(own_captured_moved_from_3755, block_body_form_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  var q := move p; "
        "  var f := lambda(): int => do var t := p.V(); return t; end; "
        "  _ := f(); "
        "  _ := move q; "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// The capture appears ONLY under an `if` inside a block-bodied lambda — the
// control-flow-nested shape #2181 had to widen the collector for. It must
// reach this rule too.
BEZEL_TEST(own_captured_moved_from_3755, capture_under_if_in_block_body_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run(flag: bool) "
        "  var p := new R(); "
        "  var q := move p; "
        "  var f := lambda(): int => do "
        "    if flag then return p.V(); end; "
        "    return 0; "
        "  end; "
        "  _ := f(); "
        "  _ := move q; "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// A NESTED lambda: the moved-from binding is captured by an inner closure
// declared inside an outer one. The binding is an enclosing-METHOD binding at
// both depths, so it is equally unreadable.
BEZEL_TEST(own_captured_moved_from_3755, nested_lambda_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take() "
        "  var p := new R(); "
        "  var q := move p; "
        "  this.h := lambda(): int => do "
        "    var g := lambda(): int => p.V(); "
        "    return g(); "
        "  end; "
        "  _ := move q; "
        "end Take; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// The lambda in CALL-ARGUMENT position (a function-typed parameter). The
// argument closure is created at this statement, where `p` is moved-from.
BEZEL_TEST(own_captured_moved_from_3755, call_argument_position_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class C "
        "  public static method Go(f: () -> int): int return f(); end Go; "
        "  public static method Run(): int "
        "    var p := new R(); "
        "    var q := move p; "
        "    var n := Go(lambda(): int => p.V()); "
        "    _ := move q; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// The lambda in RETURN position — the escaping form. `checkClosureEscapesOnReturn`
// classifies the capture's SOURCE liveness; it says nothing about the binding
// already being moved-from, so this shape needs the read rule as well.
BEZEL_TEST(own_captured_moved_from_3755, return_position_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class C "
        "  public static method Make(): () -> int "
        "    var p := new R(); "
        "    var q := move p; "
        "    _ := move q; "
        "    return lambda(): int => p.V(); "
        "  end Make; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// An IIFE — the closure is the callee of the call that invokes it. The walk
// must flip into the lambda body on the callee edge too (#1967's shape).
BEZEL_TEST(own_captured_moved_from_3755, iife_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  var q := move p; "
        "  var n := (lambda(): int => p.V())(); "
        "  _ := n; "
        "  _ := move q; "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMovedFromCapture(r, "p"));
}

// --- The end-to-end oracle: the real driver must REJECT the issue's repro ---
//
// The parent COMPILED this program and RAN it, printing a garbage int, so
// `r.ran` is TRUE without the fix and FALSE with it. That is what makes this
// row load-bearing rather than a restatement of the unit rows above: it drives
// `Driver::compileFile` + clang link + native exec, the same path the issue
// was measured on.
//
// Oracle note (§R12-G): the observable here is a COMPILE REJECTION, not a
// crash signature and not a leak, so it is oracle-independent — the plain and
// the ASan (`BEZEL_AOT_FORCE_SANITIZE=address`) passes see the identical
// front-end verdict, and no program is produced under either.
BEZEL_TEST(own_captured_moved_from_3755, aot_issue_repro_rejected_no_program_28_3) {
    JitResult r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class Box "
        "  public h: (() -> int)?; "
        "  public method Take(p: owned R) "
        "    var q := move p; "
        "    this.h := lambda(): int => p.V(); "
        "    _ := move q; "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    box.Take(new R()); "
        "    Console.WriteLine(box.h!().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // Without the fix the program compiles and executes (ran == true) and
    // prints a garbage int read out of freed storage.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.empty());
}

// --- POS: shapes that must keep compiling (the false-reject guard) ---------

// The ordinary capture of a LIVE owned binding — the whole point of §28.3's
// "reads borrow". Nothing was moved; nothing may be rejected.
BEZEL_TEST(own_captured_moved_from_3755, live_owned_capture_accepted_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  using var p := new R(); "
        "  var f := lambda(): int => p.V(); "
        "  _ := f(); "
        "end Run; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REASSIGNED after the move: `p := new R();` restores the binding to Owned, so
// the later capture reads a live binding. §28.8.1: a moved-from binding is
// unreadable "until reassigned" — this is the "until".
BEZEL_TEST(own_captured_moved_from_3755, reassigned_after_move_capture_accepted_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  var q := move p; "
        "  p := new R(); "
        "  var f := lambda(): int => p.V(); "
        "  _ := f(); "
        "  _ := q; "
        "  p.Dispose(); "
        "end Run; "));
    // Pin the bug (§28.3 K3.13 marker economy / §17.13 K3.9 discard):
    // without this assertion the test passes vacuously on a narrow
    // `_ := move q;` program, because the original narrow oracle
    // (rejectsMovedFromCapture, "p") does not see the K3.13 marker-economy
    // diagnostic on `q`. §28.8.1: a moved-from binding is unreadable until
    // reassigned — `p` is reassigned to a fresh `new R()` on the line
    // before the capture, so the capture reads a live binding.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(rejectsMovedFromCapture(r, "p"));
}

// SHADOWING by a lambda PARAMETER of the same name. The inner `p` resolves to
// the lambda's own parameter symbol, which is not a method-scope binding at
// all. A name-keyed implementation would reject this valid program.
BEZEL_TEST(own_captured_moved_from_3755, lambda_param_shadow_accepted_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  var q := move p; "
        "  var f := lambda(p: int): int => p + 1; "
        "  _ := f(41); "
        "  _ := q; "
        "end Run; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(rejectsMovedFromCapture(r, "p"));
}

// SHADOWING by a lambda-BODY LOCAL of the same name — the other half of the
// shadowing axis.
BEZEL_TEST(own_captured_moved_from_3755, lambda_body_local_shadow_accepted_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  var q := move p; "
        "  var f := lambda(): int => do var p := 7; return p; end; "
        "  _ := f(); "
        "  _ := q; "
        "end Run; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(rejectsMovedFromCapture(r, "p"));
}

// A BORROWED (plain, non-`owned`) parameter captured by a closure carries no
// ownership obligation and is never MovedFrom, so THIS rule stays silent.
//
// The original row stored the closure into a FIELD and asserted
// `!hasErrors()`. `ac2c7c6e` (#4709) then made that shape a §27.5 escape —
// correctly: `p`'s source is the call, which the field necessarily outlives,
// and a function type announces no source set, so no caller can bound the
// escaping borrow. The row was red not because its axis changed but because
// its oracle was over-broad: `!hasErrors()` catches EVERY rule, including ones
// the row is not about. Its sibling `lambda_body_local_shadow_accepted_28_3`
// got this right by also asserting `!rejectsMovedFromCapture(...)`.
//
// Split in two, so each row is load-bearing on exactly one rule.

// (a) The original axis, on a NON-escaping capture: a plain borrow parameter
// captured by a closure that stays local is not a moved-from capture.
BEZEL_TEST(own_captured_moved_from_3755, borrow_param_capture_accepted_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(p: R) "
        "  var f := lambda(): int => p.V(); "
        "  _ := f(); "
        "end Take; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(rejectsMovedFromCapture(r, "p"));
}

// (b) The field-store shape #4709 now rejects. Pinned on the §27.5 escape
// rule specifically — and on `rejectsMovedFromCapture` being FALSE, which is
// the division of labour: the escape rule owns this, the moved-from rule does
// not, and a future widening of either must not silently take it over.
BEZEL_TEST(own_captured_moved_from_3755,
           borrow_param_capture_field_store_escapes_27_5) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(p: R) "
        "  this.h := lambda(): int => p.V(); "
        "end Take; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(
        r, "a plain (borrowing) parameter"));
    BEZEL_EXPECT_FALSE(rejectsMovedFromCapture(r, "p"));
}

// --- The boundary row: the literal is created BEFORE the move -------------
//
// §27.5's capture sharing is by binding, so a closure created before the move
// and invoked after it would read the moved-from binding at INVOCATION. That
// shape is NOT this rule's business — `p` is Owned at the literal, so the read
// check is correctly silent — and the spec assigns it to the consume-site
// freeze instead (§28.3: "A borrow is *live* at a consume site when some path
// from the site reaches a later use of it … or through a closure value that is
// itself still live"). Measured: the freeze already rejects it, with its own
// diagnostic, on the parent AND after this fix. This row pins that division of
// labour, so a later widening of the read rule to the invocation position
// cannot silently double-report or take this case over unnoticed.
BEZEL_TEST(own_captured_moved_from_3755, literal_before_move_stays_freeze_not_this_rule_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  var f := lambda(): int => p.V(); "
        "  var q := move p; "
        "  _ := f(); "
        "  _ := move q; "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The consume-site freeze owns this shape, not the capture-read rule.
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "consume-site freeze"));
    BEZEL_EXPECT_FALSE(rejectsMovedFromCapture(r, "p"));
}

// --- The direct-read twin, unchanged --------------------------------------
//
// The same read OUTSIDE a closure was already rejected before this fix, with
// its own §28.3 wording. Pinned here so a future refactor that unifies the two
// messages has to update this row deliberately rather than by accident.
BEZEL_TEST(own_captured_moved_from_3755, direct_read_after_move_still_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  var q := move p; "
        "  _ := p.V(); "
        "  _ := move q; "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "owned value in `p` was already moved out at this point (§28.3)"));
}


// =========================================================================
// The same-STATEMENT leg — §22.5 *Evaluation order* / §28.3 (review
// follow-up to the first landing of this fix).
// =========================================================================
//
// The first landing consulted `states_` at STATEMENT ENTRY only, so a consume
// performed by ANOTHER PART OF THE SAME STATEMENT was invisible to it and
// `Go(move p, lambda(): int => p.V())` compiled clean. Measured on that build
// (e367a45), with `--sanitize=address`, every one of the five shapes below
// reports
//
//   ERROR: AddressSanitizer: heap-use-after-free ... READ of size 4
//
// and on the plain build prints a garbage int (`-1496813841`, `-1997385136`,
// `-1247672283`, `-317835060`, `-973740565` respectively) instead of 5.
//
// BOTH argument orders are unsafe, which is why the rule is order-independent
// rather than a left-to-right "already moved by an earlier argument" test:
// §22.5 fixes when the argument EXPRESSIONS run, but the closure is INVOKED
// by the callee, after every argument has been evaluated and (here) after the
// callee has already disposed the transferred value. The capture-then-move
// spelling is §28.3's consume-site freeze on its face — "A borrow is *live* at
// a consume site when some path from the site reaches a later use of it — …
// through a closure value that is itself still live" — but that check's
// implementation only sees closures held by LOCALS, so an argument-position
// closure temporary escapes it. One rule covers both directions.

// A. `move` in an EARLIER argument than the capturing lambda.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_move_arg_then_capture_arg_rejected_22_5) {
    auto r = parseAndCheckOwnership(wrap2(
        "    var p := new R(); "
        "    return Go(move p, lambda(): int => p.V()); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsSameStatementTransfer(r, "p"));
}

// B. `move` in a LATER argument than the capturing lambda — equally a
// heap-use-after-free, so equally rejected. This is the row that makes the
// rule order-INdependent; a "moves of earlier arguments only" implementation
// passes A and fails here.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_capture_arg_then_move_arg_rejected_22_5) {
    auto r = parseAndCheckOwnership(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class C "
        "  public static method GoR(f: () -> int, x: owned R): int "
        "    x.Dispose(); return f(); "
        "  end GoR; "
        "  public static method Run(): int "
        "    var p := new R(); "
        "    return GoR(lambda(): int => p.V(), move p); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsSameStatementTransfer(r, "p"));
}

// NAMED arguments, transfer named first. §22.5: "naming an argument changes
// which parameter it binds to, never when it runs" — so the named spelling is
// the same hazard and must reject identically.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_named_args_move_first_rejected_22_5) {
    auto r = parseAndCheckOwnership(wrap2(
        "    var p := new R(); "
        "    return Go(x := move p, f := lambda(): int => p.V()); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsSameStatementTransfer(r, "p"));
}

// NAMED arguments, REORDERED so the lambda is written first and binds the
// SECOND parameter. Textual order and parameter order disagree here, which is
// exactly the case §22.5's sentence is about.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_named_args_lambda_first_rejected_22_5) {
    auto r = parseAndCheckOwnership(wrap2(
        "    var p := new R(); "
        "    var n := Go(f := lambda(): int => p.V(), x := move p); "
        "    return n; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsSameStatementTransfer(r, "p"));
}

// A CONSTRUCTOR's `owned` argument is the same consume position as a method
// call's, and reaches the rule through the NewExpr argument walk.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_ctor_owned_arg_rejected_22_5) {
    auto r = parseAndCheckOwnership(wrap2(
        "    var p := new R(); "
        "    using var h := new Holder(move p, lambda(): int => p.V()); "
        "    return h.n; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsSameStatementTransfer(r, "p"));
}

// The end-to-end oracle for the same-statement leg: the real driver must
// produce no program. On e367a45 this compiled, ran, and printed garbage.
BEZEL_TEST(own_captured_moved_from_3755, aot_same_stmt_move_arg_rejected_no_program_22_5) {
    JitResult r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class C "
        "  public static method Go(x: owned R, f: () -> int): int "
        "    x.Dispose(); return f(); "
        "  end Go; "
        "  public static method Run(): int "
        "    var p := new R(); "
        "    return Go(move p, lambda(): int => p.V()); "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.empty());
}

// --- POS controls for the same-statement leg ------------------------------

// The statement consumes `p` but the lambda captures a DIFFERENT binding `q`.
// Only the consumed binding's captures are rejected. Runs and prints 5.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_capture_of_other_binding_accepted_22_5) {
    auto r = parseAndCheckOwnership(wrap2(
        "    var p := new R(); "
        "    using var q := new R(); "
        "    return Go(move p, lambda(): int => q.V()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(rejectsSameStatementTransfer(r, "p"));
}

// The statement consumes `p` and the lambda captures NOTHING. Accepted.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_no_capture_accepted_22_5) {
    auto r = parseAndCheckOwnership(wrap2(
        "    var p := new R(); "
        "    return Go(move p, lambda(): int => 42); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The consumed thing is a FRESH TEMPORARY, not the captured binding — the
// captured `using var p` is untouched by the transfer. Accepted; prints 5.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_consumed_temp_not_captured_binding_accepted_22_5) {
    auto r = parseAndCheckOwnership(wrap2(
        "    using var p := new R(); "
        "    return Go(new R(), lambda(): int => p.V()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(rejectsSameStatementTransfer(r, "p"));
}

// INERTNESS control: a statement that consumes `p` and contains no lambda
// literal at all is untouched — the recorder is gated on the statement having
// a lambda, so nothing is even collected here.
BEZEL_TEST(own_captured_moved_from_3755, same_stmt_consume_without_any_lambda_accepted_22_5) {
    auto r = parseAndCheckOwnership(wrap2(
        "    var p := new R(); "
        "    return Keep(move p); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// =========================================================================
// Expression-level MUTUALLY EXCLUSIVE branches — §28.3 per-path obligations
// (second review follow-up).
// =========================================================================
//
// The same-statement leg above pools a statement's consumes and its lambda
// literals. That pooling must be PER PATH: §28.3's obligations are per-path
// ("the scope-end auto-drop applies per-path, disposal runs only on the
// still-owned path"), so a consume in one arm of an expression-level branch
// and a capture in a DIFFERENT arm never co-execute and must not be paired.
//
// Statement-level control flow needed no such treatment — the walker returns
// early for IfStmt/WhileStmt/ForStmt/CaseStmt/TryStmt at the top level and
// each nested statement gets its own `checkCapturedOwnedConsumeInStmt` call
// with fresh state. The two EXPRESSION nodes with mutually exclusive
// alternatives are `ConditionalExpr` (the §20 if-atom; an elif chain nests
// inside its `elseValue`) and `CaseExpr`. Enumerated from the AST, those two
// are the whole set: the short-circuiting `??` / `and` / `or` and the
// null-conditional accesses have a conditional RIGHT side but an
// unconditional LEFT one, so their parts CAN co-execute and the flat pairing
// is already exact for them.

// UPDATED by #3989/#3990 (§28.3 K7.4, issue #3999): the four rows below
// ("...accepted_28_3" / "aot_...runs_and_reads_live_binding_28_3") used to
// assert this program compiled clean and ran to 5. It does not, safely: with
// `flag = true` (never exercised by any row in this file — every accept row
// and `wrap3`'s own `Main` only ever call `Run(false)`) `p` is genuinely
// moved into `Consume`, which disposes it, and `Run`'s own end-of-method
// scope-end auto-drop — which, before #3989/#3990, never learned about that
// move because `recordMovesIn` had no `ConditionalExpr`/`CaseExpr` arm at
// all — disposes the SAME (already-freed) `R` a second time. Measured
// (`--sanitize=address`, `Run(true)`): heap-use-after-free, `free` inside
// `bz$M.C$Consume`, then a second touch inside `bz$M.C$Run`. Confirmed
// pre-existing and unrelated to #3971: reproduces byte-identically with
// `src/ownership/{Ownership.cpp,Ownership_Checks.cpp,Ownership_impl.hpp}`
// reverted to their pre-#3971 state.
//
// #3989/#3990 gives `recordMovesIn` the missing arms, and — reusing
// `joinInto`, the SAME §28.6 issue #338 / #1626 "Owned ⊓ MovedFrom ⇒
// Owned+maybeConsumed" meet the real CFG `if`-statement join already applies
// — `p` now correctly joins to Owned+maybeConsumed after the construct, and
// reaching Run's end-of-method scan in that state is the ALREADY-SHIPPED
// §28.3 "would be dropped here without disposal" ERROR: exactly what the
// equivalent real STATEMENT form (`if flag then Sink.Take(move x); end;`, no
// else, no post-handling) has always correctly rejected. `p` genuinely IS
// unsafe to auto-drop unconditionally here, so the four rows below now pin
// the REJECTION instead of the unsound accept. #3755's OWN diagnostic
// (`rejectsSameStatementTransfer`, "closure captures ... whose owned value is
// transferred away") is a DIFFERENT, still-silent checker — capturing a
// still-Owned binding on the non-consuming path is exactly what it is
// supposed to allow, so that assertion is kept unchanged below to confirm
// #3755's own fix is not what regressed.
//
// #3999 tracks the residual: this SHAPE (a genuinely safe program per §28.3's
// own per-path reasoning) has no way to compile today because the K3.4
// marked-arm-transfer's per-path null-guarded disposal machinery
// (`dischargedByArmTransfer`) only covers `owned x` as the bare arm value,
// not a `move` embedded inside a larger arm expression like `Consume(move
// p)`. That is a feature, not a bug fix, and out of scope here.

// The reviewer's repro A: consume in `then`, capture in `else`. Both arms are
// walked (so #3755's own capture check still runs and still stays silent),
// but the construct's post-join state for `p` is Owned+maybeConsumed, and
// that state reaching Run's end-of-method scope-end scan is the correct,
// sound rejection — see the block comment above.
BEZEL_TEST(own_captured_moved_from_3755, if_expr_exclusive_arms_consume_and_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap3(
        "    var p := new R(); "
        "    var f: () -> int := (if flag then Consume(move p) "
        "                                  else lambda(): int => p.V()); "
        "    return f(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "would be dropped here without disposal"));
    BEZEL_EXPECT_FALSE(rejectsSameStatementTransfer(r, "p"));
}

// The AOT twin: no program is produced (the front end now rejects it), so
// there is nothing left to run. Before #3989/#3990 this compiled and
// `Run(false)` produced 5 — but `Run(true)`, never exercised by this file,
// heap-use-after-freed (see the block comment above); the fix trades a
// silent double-free for a compile-time error rather than an unsound accept.
BEZEL_TEST(own_captured_moved_from_3755, aot_if_expr_exclusive_arms_rejected_no_program_28_3) {
    JitResult r = runAot(wrap3(
        "    var p := new R(); "
        "    var f: () -> int := (if flag then Consume(move p) "
        "                                  else lambda(): int => p.V()); "
        "    return f(); "));
    // A bare `!r.ran` cannot tell a compile-time rejection from a runtime
    // crash, signal, timeout or forced-ASan report — all four also set
    // `ran` false (issue #3892). Pin the compile-time outcome explicitly.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.stdoutText.empty());
}

// The reviewer's repro B: the case-expression analogue. Same verdict — the
// gap was structural, not specific to the if-atom, in both directions (the
// original hole AND its #3989/#3990 fix).
BEZEL_TEST(own_captured_moved_from_3755, case_expr_exclusive_arms_consume_and_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap3(
        "    var p := new R(); "
        "    var f: () -> int := (case flag of "
        "      true => Consume(move p), "
        "      _ => lambda(): int => p.V() "
        "    ); "
        "    return f(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "would be dropped here without disposal"));
    BEZEL_EXPECT_FALSE(rejectsSameStatementTransfer(r, "p"));
}

BEZEL_TEST(own_captured_moved_from_3755, aot_case_expr_exclusive_arms_rejected_no_program_28_3) {
    JitResult r = runAot(wrap3(
        "    var p := new R(); "
        "    var f: () -> int := (case flag of "
        "      true => Consume(move p), "
        "      _ => lambda(): int => p.V() "
        "    ); "
        "    return f(); "));
    // A bare `!r.ran` cannot tell a compile-time rejection from a runtime
    // crash, signal, timeout or forced-ASan report — all four also set
    // `ran` false (issue #3892). Pin the compile-time outcome explicitly.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.stdoutText.empty());
}

// THREE arms, the consume and the capture two arms apart with an unrelated
// arm between them — the arm INDEX has to be compared, not merely "is this
// inside some arm" (#3755's own concern). The SAME §28.3 K7.4 join applies
// regardless of arm count: `p` is consumed on the `true` arm and not on
// either other arm, so it is Owned+maybeConsumed after the construct and
// Run's end-of-method scope-end scan correctly rejects it (see the block
// comment above; this row used to assert `Run(false)` — selecting the
// middle, capture-free arm — ran to 7, which said nothing about the
// `true`-arm double-free `Run(true)` never got to exercise).
BEZEL_TEST(own_captured_moved_from_3755, case_expr_three_arms_consume_and_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap3(
        "    var p := new R(); "
        "    var f: () -> int := (case flag of "
        "      true => Consume(move p), "
        "      false => lambda(): int => 7, "
        "      _ => lambda(): int => p.V() "
        "    ); "
        "    return f(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "would be dropped here without disposal"));
    BEZEL_EXPECT_FALSE(rejectsSameStatementTransfer(r, "p"));
}

// --- The rejecting side of the same axis ---------------------------------

// The CONDITION consumes and BOTH arms capture. A condition runs on every
// path through the construct, so it co-executes with either arm — this must
// still reject. (This is the row that fails if the scoping is applied to the
// condition as though it were an arm.)
BEZEL_TEST(own_captured_moved_from_3755, if_expr_condition_consumes_both_arms_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap3(
        "    var p := new R(); "
        "    var f: () -> int := (if ConsumeB(move p) "
        "                         then lambda(): int => p.V() "
        "                         else lambda(): int => p.V()); "
        "    return f(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsSameStatementTransfer(r, "p"));
}

// Consume and capture in the SAME arm — they co-execute, so the arm scoping
// must not swallow the rejection.
BEZEL_TEST(own_captured_moved_from_3755, if_expr_same_arm_consume_and_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap3(
        "    var p := new R(); "
        "    var n: int := (if flag then Go(move p, lambda(): int => p.V()) "
        "                           else 0); "
        "    return n; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsSameStatementTransfer(r, "p"));
}

// The consume sits in the ARGUMENT LIST, OUTSIDE the branch; the capture is
// inside one arm. The consume is unconditional relative to the branch, so it
// co-executes with every arm and the rejection stands.
BEZEL_TEST(own_captured_moved_from_3755, consume_outside_branch_capture_inside_arm_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap3(
        "    var p := new R(); "
        "    return Go(move p, (if flag then lambda(): int => p.V() "
        "                               else lambda(): int => 0)); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsSameStatementTransfer(r, "p"));
}
