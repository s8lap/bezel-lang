// Ownership — fix #4311: a Pattern-A `Task<T:class>` async producer's
// non-`await` `return` of a bare `IdentifierExpr` naming a BORROWED local, a
// plain (non-`owned`) parameter, or an implicit-`this` field read was not
// rejected by `fix #4285` (which only recognizes the `MemberAccessExpr` /
// `NullCondMemberExpr` / `IndexExpr` AST shapes) — `fix #4285` deliberately
// deferred the bare-identifier shape here because distinguishing a genuinely
// owned local/parameter from a borrowed one needs the ownership pass's own
// flow state (`states_` / `lookupState`), which codegen does not carry.
//
// Spec (revision v0.298):
//   §30.5 "Result lifetime and task disposal", *Producer-side return-type
//   rules*, **Pattern A result slots are of three kinds**, kind 1: "A fresh
//   owned value (a `return` of an owned expression): the task owns the
//   result and its destructor disposes it directly." No fourth kind exists,
//   and a bare (non-`await`) return must satisfy kind 1.
//   §28.3 "Local variables, ownership, and borrows", *Producers* table: a
//   field/property/indexer read (including the bare-identifier
//   implicit-`this` spelling) is Borrowed; a plain (non-`owned`) parameter
//   carries no obligation to transfer.
//
// MEASURED (§30-AMENDED amplification, on the r18-c4 base `f99d7c4`, BEFORE
// this fix — the SAME base `fix #4285`'s own reviewer used): the shape
//
//   var borrowed := h.item;      // h.item: a plain (non-owned) field — a
//                                 // borrow per §28.3 Producers
//   return borrowed;             // IdentifierExpr — NOT covered by #4285
//
// amplified 300x under ASan reproduces the SAME heap-use-after-free #4285
// fixed for the MemberAccessExpr shape — identical 3-frame signature
// (`bz$elemDispose` <- `bz_task_free` <- the async resume frame), freed by
// the real owner's explicit `.Dispose()`. This file's compile-time rejection
// rows do not need the 300-iteration loop themselves (the rejection is
// unconditional and producer-side, proven by the "never awaited" row below,
// mirroring `fix #4285`'s own `reject_unconditional_even_when_never_
// awaited_30_5`); the loop was needed only to force the PRE-fix runtime UAF
// to manifest reliably under ASan's allocator reuse, exactly as #4285's file
// header explains for its own rows.
//
// THE FIX (Ownership.cpp, new `checkAsyncPatternABorrowedIdentifierReturn`,
// called from `handleReturn` / `runExprBodyOwningReturnCheck` beside
// `checkOwnedFieldReturn`): for a PLAIN `Task<T>` (never `OwningTask` —
// Pattern B is untouched, see the boundary row below) async producer's
// bare-`IdentifierExpr` non-`await` return, consult `lookupState`/the
// resolved `ParameterSymbol`/`FieldSymbol` to tell a genuinely owned source
// (a plain `var` tracked `Owned`, an `owned`/`out owned` parameter) apart
// from a borrow (a local tracked `Borrowed`, a plain parameter, an
// implicit-`this` field read) and reject only the latter — using the EXACT
// same diagnostic wording `fix #4285` uses, so the two AST shapes of the
// same §30.5 kind-1 violation read as ONE rule to the user, not two.
//
// FAIL-WITHOUT / PASS-WITH (measured via commit-fix, revert-src, rebuild,
// re-run, restore-src, rebuild cycle): the 4 reject rows below
// (`reject_borrowed_local_return_30_5`, `reject_borrowed_parameter_
// return_30_5`, `reject_implicit_this_field_return_30_5`,
// `reject_unconditional_even_when_never_awaited_30_5`) compile CLEAN
// (ran == true) without this fix and are rejected with a real location and
// this fix's diagnostic text WITH it. The accept-side control rows pass
// identically on both compilers (deliberately unaffected axes — see each
// row's own comment).

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
// `fix #4285`'s own test file uses for this family's disposal question.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// A plain (non-owned, non-tag) Cell for the reject rows (the FAIL-WITHOUT
// proof does not need disposal counting — only compile-time rejection).
const char* kPlainCell =
    "class Cell public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; end Cell; ";

// A non-owned class-typed field: `Box.item` is a plain (borrow) field per
// §28.3 Producers — the exact shape `fix #4285`'s `Holder.item` repro (and
// this issue's own repro) uses.
const char* kBox =
    "class Box public item: Cell; "
    "  public constructor(c: Cell) this.item := c; end constructor; "
    "end Box; ";

// The distinguishing diagnostic-text fragment — WORDED IDENTICALLY to
// `fix #4285`'s own `kFixDiagFragment` so the two shapes of the same
// violation read as one rule, not two.
constexpr const char* kFixDiagFragment =
    "§30.5 Pattern A) may only `return` an OWNED expression";
}  // namespace

// ---------------------------------------------------------------------------
// Reject rows (#4311's three named shapes).
// ---------------------------------------------------------------------------

// The issue's own repro: a plain `var` bound to a borrow, returned bare.
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          reject_borrowed_local_return_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsync(h: Box): Task<Cell> "
        "    var borrowed := h.item; "  // Borrowed — h.item is a field read
        "    return borrowed; "         // IdentifierExpr — #4311's shape
        "  end GetAsync; "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(1); "
        "    var h := new Box(x); "
        "    using var t := C.GetAsync(h); "
        "    var y := await t; "
        "    x.Dispose(); "
        "    h.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// A plain (non-`owned`) PARAMETER returned bare — the parameter carries no
// transfer obligation (§28.3: a plain parameter is a borrow of the caller's
// argument).
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          reject_borrowed_parameter_return_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class C "
        "  public static async method GetAsync(c: Cell): Task<Cell> "
        "    return c; "  // plain parameter — a borrow
        "  end GetAsync; "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(1); "
        "    using var t := C.GetAsync(x); "
        "    var y := await t; "
        "    x.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// An implicit-`this` bare FIELD read (no `this.` / `obj.` prefix) — the same
// unconditional borrow `fix #4285` rejects via the explicit spelling, now
// reached via `exprIsOwnedFieldRead`'s OTHER recognized spelling.
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          reject_implicit_this_field_return_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class Box "
        "  public item: Cell; "
        "  public constructor(c: Cell) this.item := c; end constructor; "
        "  public async method GetAsync(): Task<Cell> "
        "    return item; "  // bare field name, implicit `this.item`
        "  end GetAsync; "
        "end Box; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(1); "
        "    var h := new Box(x); "
        "    using var t := h.GetAsync(); "
        "    var y := await t; "
        "    x.Dispose(); "
        "    h.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// The rejection is PRODUCER-SIDE and unconditional: it fires from
// `GetAsync`'s own body regardless of whether any caller ever awaits it —
// mirrors `fix #4285`'s own `reject_unconditional_even_when_never_
// awaited_30_5` row.
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          reject_unconditional_even_when_never_awaited_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsync(h: Box): Task<Cell> "
        "    var borrowed := h.item; "
        "    return borrowed; "
        "  end GetAsync; "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(1); "
        "    var h := new Box(x); "
        "    using var t := C.GetAsync(h); "  // never awaited
        "    x.Dispose(); "
        "    h.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// ---------------------------------------------------------------------------
// Accept-side controls (§15 / LANE_RULES: the reject-side FAIL-WITHOUT proof
// says nothing about programs that must keep compiling).
// ---------------------------------------------------------------------------

// A bare IdentifierExpr return of a still-OWNED local (a plain `var`, kind
// 1's genuine source) must keep compiling and disposing correctly — this is
// EXACTLY the axis `fix #4285` left conservative (its own
// `accept_bare_owned_local_return_unaffected_30_5` row), now exercised
// against THIS fix's new check to prove no regression. Loop-amplified (50x).
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          accept_owned_local_return_unaffected_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method MakeAsync(): Task<Cell> "
        "    var c := new Cell(); "
        "    return c; "  // owned local — admitted
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do using var t := C.MakeAsync(); var c := await t; end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// An `owned` PARAMETER returned bare — genuinely owned (transferred into the
// callee), a NEW accept control this fix's parameter arm must admit (#4285's
// own grid tested only the owned-LOCAL accept shape, not owned-parameter).
// Loop-amplified (50x).
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          accept_owned_parameter_return_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method PassThroughAsync(owned c: Cell): "
        "      Task<Cell> "
        "    return c; "  // owned parameter — admitted
        "  end PassThroughAsync; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do "
        "        var x := new Cell(); "
        "        using var t := C.PassThroughAsync(move x); "
        "        var c := await t; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// Pattern B (`OwningTask<T>`) is structurally out of this fix's gate (scoped
// to the PLAIN `Task` head only), so the identical borrowed-identifier-
// return shape must still be rejected by whichever rule already covered it,
// UNCHANGED — proven by asserting the OTHER rule's own diagnostic text
// fires, and this fix's text does NOT (mirrors `fix #4285`'s own
// `accept_pattern_b_borrow_return_rejected_by_existing_rule_30_5`).
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          accept_pattern_b_borrowed_identifier_rejected_by_existing_rule_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsyncB(h: Box): OwningTask<Cell> "
        "    var borrowed := h.item; "
        "    return borrowed; "
        "  end GetAsyncB; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may not be returned through the owning-type return"));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, kFixDiagFragment));
}

// A legitimate kind-2 chained return (`return await <owned Pattern A
// source>`) is untouched — the checked expr is a `UnaryExpr("await")`, never
// an `IdentifierExpr`, so this fix's `dynamic_cast<IdentifierExpr>` guard
// never matches it. Loop-amplified (50x).
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          accept_chained_await_return_unaffected_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsync(): Task<Cell> "
        "    return new Cell(); "
        "  end LoadAsync; "
        "  public static async method ChainAsync(): Task<Cell> "
        "    using var inner := C.LoadAsync(); "
        "    return await inner; "  // kind 2: chained slot
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do using var t := C.ChainAsync(); var c := await t; end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// #4311 / #4319 boundary: the two fixes pull in OPPOSITE directions on the
// SAME check (`checkAsyncPatternABorrowedIdentifierReturn` rejects more;
// `checkOwnedFieldReturn`'s new carve-out accepts more) and do not collide —
// they gate on DISJOINT AST node types (`IdentifierExpr` vs.
// `UnaryExpr("move")`), so a program exercising both shapes in the same
// module gets the CORRECT verdict on each independently.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_pattern_a_borrowed_identifier_return_4311,
          boundary_4311_reject_and_4319_accept_coexist) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class Box "
        "  public owned item: Cell; "
        "  public constructor(owned c: Cell) this.item := c; end "
        "      constructor; "
        "end Box; "
        "class C "
        // #4311's reject shape: bare identifier naming a borrowed local.
        "  public static async method GetBorrowed(h: Box): Task<Cell> "
        "    var borrowed := h.item; "
        "    return borrowed; "
        "  end GetBorrowed; "
        // #4319's accept shape: `move` of a genuinely owned field.
        "  public static async method GetOwned(h: Box): Task<Cell> "
        "    return move h.item; "
        "  end GetOwned; "
        "end C;");
    // The WHOLE module fails to compile (GetBorrowed's rejection), but the
    // rejection is #4311's diagnostic, not #1005's — proving GetOwned's
    // `move` return was never the reason, and #4311's check did not
    // misfire on GetOwned's `move`-shaped return either (a UnaryExpr, not
    // an IdentifierExpr).
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(
        r, "cannot return an owned field's `move`d value"));
}
