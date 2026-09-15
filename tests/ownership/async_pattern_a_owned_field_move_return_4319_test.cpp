// Ownership — fix #4319: `checkOwnedFieldReturn` (§28.6/#1005) false-rejected
// `return move <owned field>;` from a Pattern-A `Task<T>` (class T) async
// producer, even though the value is a genuinely OWNED expression per §30.5
// kind 1.
//
// Spec (revision v0.298):
//   §30.5 "Result lifetime and task disposal", *Producer-side return-type
//   rules*, **Pattern A result slots are of three kinds**, kind 1:
//     "A fresh owned value (a `return` of an owned expression): the task
//      owns the result and its destructor disposes it directly."
//   §28.3 "Local variables, ownership, and borrows", *Producers* table:
//     "`move obj.f` where `f` is `owned`: Owned."
//   §28.6 (#1005): "An owned field's value may not be returned from a method
//   unless the return type is also declared `owned`" — the SYNC rule this
//   fix does not touch.
//
// THE DEFECT. `MethodOwnership::checkOwnedFieldReturn`'s non-`owned`-return
// path (Ownership.cpp, §28.6/#1005) unconditionally rejects `move <owned
// field>` whenever the declared return type does not carry the literal
// `owned` keyword — with no carve-out for a Task-family declared return type
// the way its sibling declaration-site check (`staticReturnTypeRejected`,
// `if (common::isTaskFamilyHead(head)) return {};  // §28.3 v0.190 carve`)
// already has. But a plain `Task<T>` async producer's non-`await` return
// value is NOT the method's literal return type (`Task<T>`) — it is the
// PRODUCED value (`T`), and §30.5 kind 1 makes an owned `T` value exactly
// the admitted shape: the freshly-minted TASK becomes the field value's new
// owner and disposes it at its own teardown, so the value is not left "with
// no owner" the way the #1005 rule (written for the sync case) assumes.
//
// MEASURED, this run's `c4-g-work` branch (`scripts/agent_check.sh --probe`,
// pre-fix): the exact repro below rejects with the #1005 diagnostic text
// ("cannot return an owned field's `move`d value through a non-`owned`
// return"), even though the workaround `var x := move h.item; return x;`
// (an IdentifierExpr, which `checkOwnedFieldReturn` never inspects) compiles
// clean — confirming the rejection is a false positive, not a real §28.6
// violation.
//
// THE FIX (Ownership.cpp, `checkOwnedFieldReturn`'s non-`owned`-return path):
// when the enclosing method is `isAsync_` and its declared return type's
// head is the PLAIN `Task` name (never `OwningTask` — Pattern B is a
// separate, unmeasured question, see issue 4319's follow-up), admit the
// `move <owned field>` return instead of reporting. Scoped narrowly to avoid
// silently broadening Pattern B's producer-side rules.
//
// FAIL-WITHOUT / PASS-WITH (measured via commit-fix, revert-src, rebuild,
// re-run, restore-src, rebuild cycle): the one reject-shaped row below
// (`accept_move_owned_field_return_task_pattern_a_4319`, which is a
// COMPILE-CLEAN + dispose-exactly-once ACCEPT row post-fix) compiles CLEAN
// pre-fix... no — compiles WITH the #1005 error pre-fix and compiles clean
// (and runs dispose-exactly-once) post-fix. The accept-side/boundary rows
// pass identically on both compilers (see each row's own comment).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Cell: a class with a static counter (ctor ++, dtor --) for exact,
// deterministic dispose-exactly-once verification — the same idiom
// `fix #4285`'s own test file (async_pattern_a_producer_borrow_return_4285_
// test.cpp) and tests/codegen/async_task_result_disposal_1503_test.cpp
// (#1503) use for this family's disposal question.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// A genuinely OWNED field: `Box.item` is `owned` and constructed through an
// `owned` parameter (not a borrow-into-owned-field setup mistake) — the
// exact shape issue 4319's repro uses.
const char* kBox =
    "class Box public owned item: Cell; "
    "  public constructor(owned c: Cell) this.item := c; end constructor; "
    "end Box; ";

// The #4319 fix's admitted shape carries no NEW diagnostic text (it is an
// ACCEPT, not a reject) — so there is no shared fragment constant to key
// off here, unlike the sibling #4311 file. The pre-fix REJECTION text (kept
// for the FAIL-WITHOUT proof and the sync-arm regression control) is #1005's
// pre-existing wording.
constexpr const char* k1005DiagFragment =
    "cannot return an owned field's `move`d value through a non-`owned` "
    "return";
}  // namespace

// ---------------------------------------------------------------------------
// Main repro row (#4319): `return move h.item;` from a Pattern-A `Task<T>`
// async producer must compile and dispose the produced Cell exactly once at
// the task's own teardown. Loop-amplified (50x, ASan-run via the harness'
// default sanitized build) so a leak or a double-free/UAF corrupting the
// counter cannot pass by accident (§30-AMENDED).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_pattern_a_owned_field_move_return_4319,
          accept_move_owned_field_return_task_pattern_a_4319) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsync(h: Box): Task<Cell> "
        "    return move h.item; "  // §30.5 kind 1: an OWNED expression
        "  end GetAsync; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do "
        "        var x := new Cell(); "
        "        var h := new Box(move x); "
        "        using var t := C.GetAsync(h); "
        "        var y := await t; "
        "        y.Dispose(); "
        "        h.Dispose(); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0: every iteration's Cell disposed
                               // exactly once
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// Boundary / accept-side controls that must not regress.
// ---------------------------------------------------------------------------

// The SYNC (non-async) #1005 rule this fix does NOT touch — the spec's own
// `Take(): Cell? => move v;` shape (tests/ownership/owned_field_return_1005_
// test.cpp's `move_owned_field_exprbody_rejected` pins the same rule; this
// row re-confirms it in THIS file, on the exact `Box`/`Cell` fixtures above,
// so the boundary between "async Task-family carve" and "sync #1005 leak" is
// visible in one place).
BEZEL_TEST(async_pattern_a_owned_field_move_return_4319,
          boundary_sync_move_owned_field_still_rejected) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class D "
        "  public method Take(h: Box): Cell? "
        "    return move h.item; "  // NOT async — the genuine #1005 leak
        "  end Take; "
        "end D;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, k1005DiagFragment));
}

// Pattern B (`OwningTask<T>`) IS NOW EXEMPTED by fix #4337 — the same §30.5
// kind 1 reasoning that applies to plain `Task<T>` (fix #4319) also applies
// to `OwningTask<T>` (fix #4337). `move h.item` returned through
// `OwningTask<Cell>` from an async body is now ADMITTED (not rejected).
// The accept behaviour is pinned by the dedicated fix #4337 test file
// (tests/ownership/async_owning_task_owned_field_move_return_4337_test.cpp);
// this row simply confirms the #4319 fix does NOT regress the accept.
BEZEL_TEST(async_pattern_a_owned_field_move_return_4319,
          boundary_owningtask_move_owned_field_now_admitted_by_4337) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsyncB(h: Box): OwningTask<Cell> "
        "    return move h.item; "  // now admitted by fix #4337
        "  end GetAsyncB; "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(); "
        "    var h := new Box(move x); "
        "    using var t := C.GetAsyncB(h); "
        "    var y := await t; "
        "    y.Dispose(); "
        "    h.Dispose(); "
        "    return Cell.n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
}

// A bare (non-`move`) read of the owned field through the SAME Pattern-A
// `Task<Cell>` return is `fix #4285`'s OWN rejection (a MemberAccessExpr top-
// level return) — unaffected by this fix, which only ever touches the
// `UnaryExpr("move")` arm of `checkOwnedFieldReturn`. Confirms the two fixes
// do not collide: #4319 widens the ACCEPT set for the `move` shape; #4285's
// existing rejection of the bare-read shape is untouched.
//
// UNLIKE this file's other boundary rows, this one needs a `Main` method:
// `checkOwnedFieldReturn` (§1005/§2368, both asserted by the sibling rows) is
// an OWNERSHIP-PASS check and fires regardless of whether the module has an
// entry point, but `fix #4285`'s rejection lives in CODEGEN
// (`detectAsyncPatternAKind`), which this driver only reaches once the
// module has a runnable `Main` — measured directly: the SAME program without
// a `Main` produces only a "no Main method" diagnostic and never reaches
// per-method codegen at all, hiding the very diagnostic this row means to
// pin.
BEZEL_TEST(async_pattern_a_owned_field_move_return_4319,
          boundary_bare_field_read_still_rejected_by_4285) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsync(h: Box): Task<Cell> "
        "    return h.item; "  // bare read, no `move` — #4285's shape
        "  end GetAsync; "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(); "
        "    var h := new Box(move x); "
        "    using var t := C.GetAsync(h); "
        "    var y := await t; "
        "    h.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "§30.5 Pattern A) may only `return` an OWNED expression"));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, k1005DiagFragment));
}

// The legitimate `owned` return type transfer (§28.6 bullet 2) stays
// unaffected — `owned Task<Cell>` (an explicit `owned` marker on a
// mint-announcing type is "identical to" the unmarked form per §28.3's
// announcement table, issue 4319's own text) with a bare (non-`move`) field
// read is STILL rejected by the `returnOwned_` arm above in
// `checkOwnedFieldReturn` (the double-free shape #2368 closed) — this fix
// only ever touches the NON-`owned`-return `move` arm.
BEZEL_TEST(async_pattern_a_owned_field_move_return_4319,
          boundary_owned_marked_return_bare_read_still_rejected) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsync(h: Box): owned Task<Cell> "
        "    return h.item; "
        "  end GetAsync; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a bare read of an owned field may not be returned through an "
           "`owned` return type"));
}
