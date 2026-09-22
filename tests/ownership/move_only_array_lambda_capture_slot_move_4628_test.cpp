// Spec-derived regression for issue #4628 (spec §28.9 "Runtime-managed
// types", §15.3 "Move-only value types", v0.300; corpus row s28_9_0006).
//
// §28.9, the move-only-local-capture paragraph:
//   "Capturing a move-only local follows the same rule [as capturing an
//    owned class-typed local]: the closure captures a borrow whose source is
//    the local's enclosing scope, and may not outlive it. [...] Reads of
//    slots through the captured borrow are admitted; `move arr[i]` through
//    it is not (the closure does not own the array) — a closure that must
//    consume slot contents takes an owned-parameter argument and is invoked
//    with `move`d slots from outside."
//
// PRE-FIX: `MethodOwnership::moveOnlyArrayLeakScan` — the general move-only-
// array tracking walk (Ownership.cpp) — deliberately stops at the
// `LambdaExpr` boundary; its own comment records the assumption that
// "nothing reachable only through a lambda body can transfer or slot-move an
// outer array". That assumption is false: `move arr[i]` inside a lambda body
// that captures `arr` from the enclosing scope compiled clean, letting the
// closure silently take ownership of a slot from an array it does not own —
// e.g. the array's own later use of that slot (`arr[0] := new Widget();`
// immediately after) now aliases a value the closure ALSO disposes, a
// double-free/double-use hazard the spec's rejection exists to prevent.
//
// FIX: a dedicated lambda-body scan (`scanLambdaBodyMoveOnlyArraySlotMoves`
// and friends, Ownership.cpp/Ownership_impl.hpp), run from
// `checkOneLambdaBodyEscapes` alongside its sibling lambda-body checks,
// rejects `move X[i]` inside a lambda body where `X` resolves to a move-only
// inline-array LOCAL declared OUTSIDE that lambda (a capture, decided by the
// same declaredInLambda span-containment test the §28.8 return-escape
// classifier above it in the file already uses).
//
// per §28.9, §15.3 (issue #4628)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// `Widget` is a disposable-shaped class (a destructor makes `[N owned]Widget?`
// a genuine move-only owning inline array, not a trivial-dispose one).
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "class C public static method Run(): int ") +
        body + " end Run; end C;";
}

}  // namespace

// =============================================================================
// REJECTS — `move arr[i]` through a closure's captured borrow of a move-only
// local declared in the ENCLOSING scope.
// =============================================================================

// §28.9 (#4628): the corpus's own repro shape (s28_9_0006) — a block-bodied
// lambda captures `arr` (declared in the enclosing method) and moves a slot
// out of it. Pre-fix this compiled clean.
BEZEL_TEST(move_only_array_lambda_capture_slot_move_4628,
           block_bodied_lambda_move_of_captured_slot_rejected_spec_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [1 owned]Widget? := [new Widget()]; "
        "var taker := lambda() => do "
        "  var w := move arr[0]; "
        "  arr[0] := new Widget(); "
        "  w!.Dispose(); "
        "end; "
        "taker(); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "does not own"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "`arr`"));
}

// §28.9 (#4628): the EXPRESSION-bodied lambda form (`lambda() => move arr[0]`,
// no `do...end`) exercises the OTHER code path in `checkOneLambdaBodyEscapes`
// (the `lam.exprBody` early-return arm) — a `move` can be the entire body of
// an expression-bodied lambda, so this shape must be reached too.
BEZEL_TEST(move_only_array_lambda_capture_slot_move_4628,
           expr_bodied_lambda_move_of_captured_slot_rejected_spec_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [1 owned]Widget? := [new Widget()]; "
        "var taker := lambda() => move arr[0]; "
        "using var w: Widget? := taker(); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "does not own"));
}

// §28.9 (#4628): the move need not be the lambda's FIRST statement — it can
// be reached after other statements, inside a nested `if`, exercising the
// `forEachNestedStmt` recursion into a nested block.
BEZEL_TEST(move_only_array_lambda_capture_slot_move_4628,
           move_of_captured_slot_inside_nested_if_rejected_spec_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [1 owned]Widget? := [new Widget()]; "
        "var flag := true; "
        "var taker := lambda() => do "
        "  if flag then "
        "    var w := move arr[0]; "
        "    w!.Dispose(); "
        "  end; "
        "end; "
        "taker(); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "does not own"));
}

// §28.9 (#4628): a SECOND-LEVEL nested lambda also captures `arr` from the
// (grandparent) method scope; the move happens inside the INNER lambda `g`,
// not the outer `f`. `g`'s own scan (reached independently via the flat
// lambda list `checkOneLambdaBodyEscapes` iterates) must catch it too.
BEZEL_TEST(move_only_array_lambda_capture_slot_move_4628,
           move_of_captured_slot_inside_doubly_nested_lambda_rejected_spec_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [1 owned]Widget? := [new Widget()]; "
        "var f := lambda() => do "
        "  var g := lambda() => do "
        "    var w := move arr[0]; "
        "    w!.Dispose(); "
        "  end; "
        "  g(); "
        "end; "
        "f(); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "does not own"));
}

// =============================================================================
// ACCEPTS — the positive controls that guard against over-rejection.
// =============================================================================

// §28.9 (#4628, corpus s28_9_0007's shape): a bare SLOT READ through the
// captured borrow — no `move` — is explicitly admitted by the same spec
// sentence ("reads of slots through the captured borrow are admitted").
BEZEL_TEST(move_only_array_lambda_capture_slot_move_4628,
           read_of_captured_slot_accepted_spec_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [1 owned]Widget? := [new Widget()]; "
        "var reader := lambda() => arr[0] is not null; "
        "var ok := reader(); "
        "using var w: Widget? := move arr[0]; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.9 (#4628): a move-only array DECLARED INSIDE the lambda's own body is
// not a capture at all — it is the lambda's own local, governed by the
// ordinary §15.3 per-slot-move rules, unaffected by this fix.
BEZEL_TEST(move_only_array_lambda_capture_slot_move_4628,
           move_of_same_lambda_declared_array_slot_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var maker := lambda() => do "
        "  var a: [1 owned]Widget? := [new Widget()]; "
        "  var w := move a[0]; "
        "  a[0] := new Widget(); "
        "  w!.Dispose(); "
        "end; "
        "maker(); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.9 (#4628): a `move` of a move-only array's slot at METHOD scope — no
// lambda involved at all — is unaffected; the new lambda-body-only check
// never runs outside a `LambdaExpr`.
BEZEL_TEST(move_only_array_lambda_capture_slot_move_4628,
           method_scope_slot_move_unaffected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [1 owned]Widget? := [new Widget()]; "
        "var w := move arr[0]; "
        "arr[0] := new Widget(); "
        "w!.Dispose(); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
