// Spec-derived regression tests for issue #3727 — the OWNED-LOCAL sibling of
// #3707 on the §28.3 assignment channel, measured as a heap-use-after-free.
//
// THE RULES, quoted from the spec (v0.286):
//
//   §28.9 "Runtime-managed types": "Capturing an *owned* class-typed local
//   does **not** transfer ownership into the closure: the capture is a borrow
//   whose source is the local's enclosing scope, borrow-state throughout — the
//   closure may not assign to, transfer from, or dispose the captured binding,
//   and may not outlive that scope (§28.3)".
//
//   §28.3 "Local variables, ownership, and borrows", sub-heading "Borrow
//   lifetime rules": "A borrow may not be assigned to a local whose scope
//   strictly encloses the borrow's source's scope ... The scope comparison is
//   structural and decidable; the diagnostic is the standard *"Borrow held by
//   `x` would outlive its source `y`."*"
//
// THE HOLE. Ownership's statement walks never enter a lambda body, so fix
// #3565 drives the assignment channel over each one with
// `checkOneLambdaBodyEscapes`, which saves, CLEARS and re-seeds `states_` and
// `localDepth_` from that ONE body. That re-seeding is what scopes the fix —
// and it is also why the channel stayed blind to a source declared OUTSIDE the
// checked body: `checkBorrowStoreEscape` resolves its source through
// `ownedLocalBorrowSource`, whose owned legs bottom out in
// `isOwnedTrackedLocal`, a `states_` lookup. An owned local of the enclosing
// method is in neither rebuilt map, so NO borrow source resolved and the check
// returned before any depth comparison, however the target was declared.
//
// fix #3707 could not cover this: its branch is gated on `isCatchBorrow`,
// which is a scope-free sema flag needing no seed at all. The fix here instead
// preserves the ENCLOSING frame's own depth map (sealed the first time
// `collectLocalDepths` runs) and re-asks the same `ownedLocalBorrowSource`
// about that frame, then re-runs the SAME depth comparison in it.
//
// MEASURED ON THE PARENT (28e5b15, which contains #3707): the first row's
// program compiles rc=0 with NO diagnostic and runs rc=139; under
// `--sanitize=address`, "heap-use-after-free ... in bz$M.C$Run", freed by
// `bz$lam$1$envdtor`.
//
// SOUNDNESS DISCIPLINE. Every positive below is paired with structurally
// similar but spec-LEGAL negatives the fix must NOT reject, and the negatives
// are run through the AOT oracle for their VALUE — an accepted program that
// stopped computing the right answer is not an acceptance (§6c: loud → quiet
// is a rejected trade). The boundaries that matter are (a) the depth
// comparison (equal or deeper target is legal), (b) the obligation (a value
// copy and a borrowed parameter carry none), and (c) the decl KIND — the
// #3707 review's lesson that a missing depth is not proof of "declared
// outside", which this fix answers by requiring a POSITIVE hit for both
// operands rather than reading a miss as a verdict.

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `R` is an ordinary class, so `using var r := new R();` is an owned
// scope-bound local (§28.3) whose capture is a §28.9 borrow.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class C ") + body +
        "  public static method Main(): int "
        "    Console.WriteLine(Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Compiles, links, runs; returns captured stdout with trailing newlines
// trimmed. A program that did not run returns a sentinel, so a row that stops
// compiling fails legibly rather than matching an empty string.
std::string runOut(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// The two METHOD-level diagnostics this fix reaches through a new frame. They
// are asserted verbatim: if a second spelling of either rule ever appears
// beside the one in `checkBorrowStoreEscape`, these rows fail.
constexpr const char* kClosureMsg =
    "closure capturing `r` is assigned to `f`, which outlives it";
constexpr const char* kPlainMsg =
    "borrow held by `holder` would outlive its source `r`";

// ------------------------------------------------------------ POSITIVES ----

// POSITIVE (§28.3 / §28.9, #3727): the filed repro. `r` is an owned
// scope-bound local of the method's inner `do ... end`; the closure capturing
// it is assigned to `f`, declared at METHOD scope, and `f` is called twice
// after the block — and therefore after `r`'s disposal.
BEZEL_TEST(lambda_body_escape_owned_3727,
           closure_assign_to_method_scope_local_rejected_28_9) {
    const std::string p = wrap(
        "  public static method Run(): int "
        "    var f := lambda(): int => 0; "
        "    var total := 0; "
        "    do "
        "      using var r := new R(); "
        "      var mk := lambda(): int => do "
        "        f := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      total := mk(); "
        "    end; "
        "    return total + f() + f(); "
        "  end Run; ");
    BEZEL_EXPECT_TRUE(rejectsWith(p, kClosureMsg));
    BEZEL_EXPECT_TRUE(rejectsWith(p, "(§28.3 / §28.9"));
}

// POSITIVE (§28.3, #3727): the PLAIN-STORE spelling of the same channel — no
// closure at all. `checkBorrowStoreEscape` picks a different diagnostic for it
// (the closure sentence would name a closure the program does not contain), so
// the two spellings can regress independently and each earns a row.
BEZEL_TEST(lambda_body_escape_owned_3727,
           plain_store_to_method_scope_local_rejected_28_3) {
    const std::string p = wrap(
        "  public static method Run(): int "
        "    var holder: R? := null; "
        "    var total := 0; "
        "    do "
        "      using var r := new R(); "
        "      var mk := lambda(): int => do "
        "        holder := r; "
        "        return 0; "
        "      end; "
        "      total := mk(); "
        "    end; "
        "    return total; "
        "  end Run; ");
    BEZEL_EXPECT_TRUE(rejectsWith(p, kPlainMsg));
    BEZEL_EXPECT_TRUE(rejectsWith(p, "(§28.3 borrow-lifetime rule"));
}

// ------------------------------------------------------------ NEGATIVES ----

// NEGATIVE (§28.3) — the target at the SAME depth as the source. Byte-for-byte
// the first positive with `f` moved inside the `do ... end`. `f` and `r` die
// at the same block end, so no call of `f` can be sequenced after `r`'s
// disposal — safe, and the fix must not reject it. This is the row that pins
// the fix did NOT become "any store to a target outside the checked lambda
// body is rejected" (the failure mode #3707's own first draft had).
BEZEL_TEST(lambda_body_escape_owned_3727,
           assign_to_same_depth_local_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(wrap(
        "  public static method Run(): int "
        "    var total := 0; "
        "    do "
        "      var f := lambda(): int => 0; "
        "      using var r := new R(); "
        "      var mk := lambda(): int => do "
        "        f := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      total := mk() + f() + f(); "
        "    end; "
        "    return total; "
        "  end Run; ")), "10");
}

// NEGATIVE (§28.3) — the target DEEPER than the source: `r` at method scope,
// `f` inside a nested block. The target dies first, so this is not an escape
// in either direction and the ordinary `td`/`sd` arm never fires either.
BEZEL_TEST(lambda_body_escape_owned_3727,
           assign_to_deeper_local_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(wrap(
        "  public static method Run(): int "
        "    using var r := new R(); "
        "    var total := 0; "
        "    do "
        "      var f := lambda(): int => 0; "
        "      var mk := lambda(): int => do "
        "        f := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      total := mk() + f() + f(); "
        "    end; "
        "    return total; "
        "  end Run; ")), "10");
}

// NEGATIVE (§28.9) — the source declared INSIDE the checked lambda body, the
// shape #3565 already governs through the body-local seed. The enclosing-frame
// retry only runs when the ordinary query came back EMPTY, so this row must be
// decided exactly as before: `f` is declared after `r` in the same body and
// invoked there, which is legal.
BEZEL_TEST(lambda_body_escape_owned_3727,
           owned_local_in_same_lambda_body_accepted_28_9) {
    BEZEL_EXPECT_EQ(runOut(wrap(
        "  public static method Run(): int "
        "    var mk := lambda(): int => do "
        "      using var r := new R(); "
        "      var f := lambda(): int => r.V(); "
        "      return f() + f(); "
        "    end; "
        "    return mk(); "
        "  end Run; ")), "10");
}

// NEGATIVE (§28.3 *Producers*) — a value-typed COPY out of the owned local, not
// a borrow of it. `n` is an `int`; the closure captures `n`, carries no borrow
// of `r`, and outliving `r` is harmless. `borrowSourceForExpr`'s
// `returnedExprMayBorrow` gate is what keeps this out, and the enclosing-frame
// seed must not have widened past it.
BEZEL_TEST(lambda_body_escape_owned_3727,
           value_copy_assign_to_method_scope_local_accepted_28_9) {
    BEZEL_EXPECT_EQ(runOut(wrap(
        "  public static method Run(): int "
        "    var f := lambda(): int => 0; "
        "    var total := 0; "
        "    do "
        "      using var r := new R(); "
        "      var n := r.V(); "
        "      var mk := lambda(): int => do "
        "        f := lambda(): int => n; "
        "        return 0; "
        "      end; "
        "      total := mk() + f() + f(); "
        "    end; "
        "    return total; "
        "  end Run; ")), "10");
}

// NEGATIVE (§28.3) — a BORROWED (non-owned) source: a plain parameter, whose
// referent is owned by the caller and outlives this frame. `ownedLocalBorrow
// Source` excludes parameters by construction (§28.8's owned-parameter
// transfer is a different rule), and seeding the enclosing frame's OWNED
// obligations cannot make a parameter one — `localIsOwnedObligation` is asked
// only about `LocalDecl` nodes. The row keeps a LIVE owned local (`spare`) in
// the very block the assignment sits in, so the enclosing-frame seed is
// NON-EMPTY and the retry really runs; the closure simply does not capture it,
// so no source resolves. That is the row that pins the seed did not turn every
// store into an outer local into a rejection.
BEZEL_TEST(lambda_body_escape_owned_3727,
           borrowed_param_capture_assign_to_method_scope_local_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class C "
        "  public static method Take(p: R): int "
        "    var f := lambda(): int => 0; "
        "    var total := 0; "
        "    do "
        "      using var spare := new R(); "
        "      var mk := lambda(): int => do "
        "        f := lambda(): int => p.V(); "
        "        return 0; "
        "      end; "
        "      total := mk() + f() + f() + spare.V(); "
        "    end; "
        "    return total; "
        "  end Take; "
        "  public static method Main(): int "
        "    using var r := new R(); "
        "    Console.WriteLine(Take(r).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "15");
}

// NEGATIVE (§28.3, the #3707 review's lesson) — a FOR-LOOP INDUCTION VARIABLE
// as the target, declared OUTSIDE the checked lambda body, with a genuine
// owned method-scope source. `cur`'s `LocalSymbol::decl` is the `ForStmt` node
// itself (src/sema/ExprTypecheck.cpp), never a `localDepth_` key in ANY frame
// — so a fix that read "no depth" as "declared outside, therefore outlives the
// source" would reject this program. It must not: `cur` dies each iteration,
// strictly before the method-scope `r`. The fix decides membership positively,
// so `enclosingFrameDepthOf(cur)` is -1 and the branch declines.
BEZEL_TEST(lambda_body_escape_owned_3727,
           forloop_var_target_outside_body_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    var items: Array<Exception> := [new InvalidOperationException(\"a\"), "
        "                                     new InvalidOperationException(\"b\")]; "
        "    using var r := new InvalidOperationException(\"owned\"); "
        "    var total := 0; "
        "    for var cur in items do "
        "      var mk := lambda(): int => do "
        "        cur := r; "
        "        return 0; "
        "      end; "
        "      total := total + mk() + cur.Message.Length; "
        "    end; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "10");
}

// NEGATIVE (§28.3, the #3707 review's lesson, second decl kind) — a CASE-ARM
// PATTERN-BOUND local as the target, declared outside the checked lambda body.
// `v`'s decl is a `BindingPattern` (or the #1271 naked anchor), the sibling
// decl kind to a `ForStmt` induction variable and equally never a
// `localDepth_` key. Same guard, same verdict: accepted, as on the parent.
BEZEL_TEST(lambda_body_escape_owned_3727,
           pattern_bound_var_target_outside_body_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; import System; "
        "public choice Box "
        "  Full(v: Exception), "
        "  Empty; "
        "end Box; "
        "class C "
        "  private static method MakeBox(): Box "
        "    return Box.Full(new InvalidOperationException(\"seed\")); "
        "  end MakeBox; "
        "  public static method Run(): int "
        "    using var r := new InvalidOperationException(\"owned\"); "
        "    var total := 0; "
        "    case MakeBox() of "
        "      Full(var v) => do "
        "        var mk := lambda(): int => do "
        "          v := r; "
        "          return 0; "
        "        end; "
        "        total := mk() + v.Message.Length; "
        "      end, "
        "      Empty => do end "
        "    end case; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "5");
}

}  // namespace
