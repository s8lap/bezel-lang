// K8.4 at the §35.5 owning-match boundary — the STATEMENT-position twin of
// #4635, plus the CLOSURE-CAPTURE route on both positions (issue #4663; spec
// §28.3 K8.3/K8.4, §35.5 *The owning match*, v0.307).
//
// THE RULE. §28.3 K8.3 puts a `case` scrutinee's boundary at "the whole
// construct — pattern bindings are borrows of the scrutinee, and the
// temporary survives arm selection, guard evaluation, and the selected arm's
// completion — in statement position exactly as in expression position".
// K8.4: "a temporary is an admissible borrow source only within its
// boundary; a borrow or frame that would escape it is rejected, never
// lifetime-extended".
//
// #4635 closed the EXPRESSION-position channel — the construct's own VALUE.
// This closes the STATEMENT-position channel: a STORE written out of a
// block-form arm's body. `outer := w;` inside `W(var w, _) => do ... end`
// writes a borrow of payload `a` into `outer`, which outlives the
// construct — the identical K8.4 escape, a different spelling.
//
// THE BUG (#4663). MEASURED on r19-c20 (this lane's rebase base, 45
// commits, #4635 already landed):
//
//   using var seed := new Widget("seed");
//   var outer := seed;
//   var o := Outer.W(new Widget("a"), new Widget("b"));
//   case move o of
//     W(var w, _) => do outer := w; end,
//     X => ()
//   end case;
//   Console.WriteLine($"outer={outer.Ping()}");   // reads payload `a` AFTER
//
// compiled clean and printed `dispose b / outer=7 / dispose seed` — payload
// `a` was never disposed at all (an LSan direct leak, confirmed under
// `--sanitize=address`), which is precisely why the #4663/#4634 ordering
// matters: the read only SURVIVES because #4617's per-slot discharge leaves
// the bound slot out of the construct-completion reclaim. Give that binding
// its §28.3 auto-drop (#4634) while this hole is open and the identical
// program becomes a genuine heap-use-after-free.
//
// THE CLOSURE ROUTE (also #4663, folded in on the same measurement pass).
// `collectCaseArmBorrowSourcesExpr`'s walker does not descend into a
// `LambdaExpr` body at all (deliberately, for an unrelated reason — a `case`
// NESTED INSIDE a closure has its own, closure-scoped obligations). Neither
// #4635's expression-position check nor this statement-position one had ever
// looked INSIDE a lambda literal that is itself the escaping value, so a
// closure CAPTURING the payload binding and then stored/returned out of the
// arm was a third, unguarded route — on BOTH positions:
//
//   var f := lambda(): int => 0;
//   case move o of
//     W(var w, _) => do f := lambda(): int => w.Ping(); end,
//     X => ()
//   end case;
//   Console.WriteLine(f().ToString());
//
// and the expression-position twin `W(var w, _) => lambda(): int =>
// w.Ping()` — both measured compiling clean with the identical leak
// signature before this fix.
//
// OBSERVABILITY. The pre-fix behaviour is a clean COMPILE, so the oracle is
// the SPECIFIC diagnostic text, never `failedAtCompileTimeWithRealLocation`
// (which would answer true for any unrelated rejection) and never `!r.ran`
// (a runtime crash also synthesizes an Error diagnostic — tests/README.md).
//
// per §28.3 K8.3/K8.4, §35.5, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The two diagnostics this fix introduces — asserting the specific text is
// load-bearing: the pre-fix compiler ACCEPTS every REJECT row below, so any
// weaker oracle would not distinguish the fix from an unrelated error.
const char* kStoreNeedle = "owning-match payload binding";
const char* kCaptureVerbNeedle = "captures owning-match payload binding";

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    return hasErrorContaining(r, needle);
}

// Mirrors owning_match_arm_borrow_escape_4635_test.cpp's own prelude exactly
// — two `owned` class payloads and a payload-free case, plus `Self()` for
// the borrow-returning-chain row and a `Holder` for the plain-field row.
const char* kPrelude =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "  public method Self(): Widget return this; end Self; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public class Holder "
    "  public Held: Widget; "
    "  public constructor(w: Widget) Held := w; end constructor; "
    "end Holder; "
    "public class Program ";

std::string prog(const std::string& mainBody) {
    return std::string(kPrelude) +
           "  public static method Main(): int " + mainBody +
           "  end Main; "
           "end Program;";
}

// ===========================================================================
// REJECT rows: the escaping STORE, every §4635-adjacent spelling #4663's own
// "scope note" predicted ("expect the statement-position check to need the
// same breadth — a store of any of those, not just the plain binding").
// ===========================================================================

// (1) THE ISSUE'S REPRO, verbatim: `case move o of` (§35.5's SECOND
// announcing scrutinee form), an arm that binds payload `a`, and a plain
// assignment of the binding OUT to a pre-existing outer local.
BEZEL_TEST(owning_match_stmt_escape_4663,
           assignment_to_outer_local_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do outer := w; end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kStoreNeedle));
}

// (2) The NAKED binding spelling (§19.3's `W(w, _)`), mirroring #4635's own
// row (3) for the expression-position check.
BEZEL_TEST(owning_match_stmt_escape_4663,
           naked_binding_spelling_rejected_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(w, _) => do outer := w; end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kStoreNeedle));
}

// (3) A BORROW-RETURNING CHAIN rooted at the binding (`w.Self()`), mirroring
// #4635's own row (4).
BEZEL_TEST(owning_match_stmt_escape_4663,
           borrow_chain_rooted_at_binding_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do outer := w.Self(); end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kStoreNeedle));
}

// (4) §35.5's FIRST announcing scrutinee form — an unbound owned temporary of
// owning-choice classification (no `case move`; the construction call itself
// mints), mirroring #4635's own row (2).
BEZEL_TEST(owning_match_stmt_escape_4663,
           unbound_temporary_scrutinee_rejected_35_5) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    case Outer.W(new Widget(\"a\"), new Widget(\"b\")) of "
        "      W(var w, _) => do outer := w; end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kStoreNeedle));
}

// (5) A STORE INTO A PLAIN (non-`owned`) FIELD — #4663's own "scope note"
// breadth: the escaping location need not be a local. A store into an
// `owned` field draws a DIFFERENT, pre-existing rejection (storing a borrow
// into an owned slot, §28.3/§28.5/§28.8) — this row is the plain-field twin
// that has no other guard.
BEZEL_TEST(owning_match_stmt_escape_4663,
           assignment_to_plain_field_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var h := new Holder(seed); "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do h.Held := w; end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"h={h.Held.Ping()}\"); "
        "    return 0; "), kStoreNeedle));
}

// (6) `return w;` from within the arm — the enclosing method itself as the
// escape channel, not an assignment target.
BEZEL_TEST(owning_match_stmt_escape_4663,
           return_from_arm_rejected_28_3_k8_4) {
    const std::string p =
        std::string(kPrelude) +
        "  public static method Take(): Widget "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do return w; end, "
        "      X => do return new Widget(\"x\"); end "
        "    end case; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine($\"got={Take().Ping()}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(p, kStoreNeedle));
}

// ===========================================================================
// REJECT rows: the CLOSURE-CAPTURE route, on BOTH positions.
// ===========================================================================

// (7) STATEMENT position: a closure assigned out of the arm CAPTURES the
// payload binding — `identifierRoot` never sees `w` directly (the escaping
// value is the LAMBDA), so this is a genuinely separate check from rows
// (1)-(6), not a redundant one.
BEZEL_TEST(owning_match_stmt_escape_4663,
           closure_capture_stmt_position_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    var f := lambda(): int => 0; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do f := lambda(): int => w.Ping(); end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(f().ToString()); "
        "    return 0; "), kCaptureVerbNeedle));
}

// (8) EXPRESSION position: the arm's own RESULT is a closure that captures
// the payload binding — #4635's own `isBorrowableType` gate never admits a
// function-typed arm result, so this shape slipped past #4635's original
// fix too, on the SAME underlying walker gap.
BEZEL_TEST(owning_match_stmt_escape_4663,
           closure_capture_expr_position_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var got := (case move o of "
        "      W(var w, _) => lambda(): int => w.Ping(), "
        "      X => lambda(): int => 0); "
        "    Console.WriteLine(got().ToString()); "
        "    return 0; "), kCaptureVerbNeedle));
}

// ===========================================================================
// ACCEPT controls: shapes the fix must NOT touch.
// ===========================================================================

// (9) A LOCAL DECLARED WITHIN THE ARM's own block, read only inside the arm.
// The new local's own scope is bounded by the arm, so it is inside K8.3's
// boundary — not an escape. Guards against the fix over-rejecting every
// store instead of only ones that outlive the arm.
BEZEL_TEST(owning_match_stmt_escape_4663,
           arm_scoped_local_still_accepted_28_3) {
    auto r = runAot(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var tmp := w; "
        "        Console.WriteLine($\"tmp={tmp.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kStoreNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("tmp=7") != std::string::npos);
}

// (10) A VALUE READ (`w.Ping()`, an `int`) assigned out — a copy, not a
// borrow. Guards against the fix flagging every store rooted at `w`
// regardless of the stored VALUE's own type.
BEZEL_TEST(owning_match_stmt_escape_4663,
           value_read_assignment_still_accepted_28_3) {
    auto r = runAot(prog(
        "    var outerN := 0; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do outerN := w.Ping(); end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outerN={outerN}\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kStoreNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("outerN=7") != std::string::npos);
}

// (11) A BORROWING match (no `move`, no owning-choice announce) — `o` keeps
// ownership, `w` is an ordinary borrow of a scrutinee that outlives the
// construct on its own terms, so the store is unrelated to K8.4 entirely.
// `owningMatchScrutineeForm` must return false and this whole check must
// no-op. Guards against the fix reaching a construct it does not own.
BEZEL_TEST(owning_match_stmt_escape_4663,
           borrowing_match_assignment_still_accepted_35_5) {
    auto r = runAot(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      W(var w, _) => do outer := w; end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kStoreNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (12) The MARKED TRANSFER spelling (`outer := move w;`) — §35.5's own
// admitted transfer, unaffected by this fix: `identifierRoot` does not peel
// a `move`-wrapped operand (it is not an identifier chain), so the marked
// form roots to nothing and is never flagged, exactly as #4635 pins for the
// expression-position transfer spelling.
BEZEL_TEST(owning_match_stmt_escape_4663,
           marked_transfer_assignment_still_accepted_35_5) {
    auto r = runAot(prog(
        "    var outer: Widget? := null; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do outer := move w; end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine((outer <> null).ToString()); "
        "    outer?.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kStoreNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
}

}  // namespace
