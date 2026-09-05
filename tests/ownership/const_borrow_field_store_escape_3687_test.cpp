// Ownership — a bare-identifier `const` re-borrow of a scope-bound owned
// local was never tracked as a live borrow SOURCE, so the §28.3 field-store
// (and outer-local-store) escape checks could never fire for it. Issue #3687.
//
// Spec, v0.291:
//   §21.1 "Variable declarations", the runtime-const "Ownership" paragraph,
//     verbatim: "...and **borrows** — the binding is then a borrow carrying
//     the initializer's source-liveness constraints, exactly as a plain
//     unmarked `var` alias: the consume-site freeze (K5) sees it as any
//     other live local borrow..."
//   §28.3 "Local variables, ownership, and borrows" — the borrowed-field /
//     borrowed-outer-local store escape rules (`checkBorrowStoreEscape`):
//     a borrow of a scope-bound owned local stored into a field, or into a
//     local with a strictly wider scope, is rejected because the store
//     outlives the source.
//
// THE BUG (measured pre-fix on HEAD f19de07):
//
//   using var localObj := new Widget();
//   var   cur := localObj; h.w := cur;   → REJECTED (§28.3, correct)
//   const cur := localObj; h.w := cur;   → ACCEPTED — a heap-use-after-free
//                                           the next time `h.w` is read
//                                           (measured under ASan:
//                                           heap-use-after-free, `free`
//                                           called from `localObj`'s
//                                           scope-end disposal).
//
// ROOT CAUSE: `MethodOwnership::handleLocalDecl` (the `var` path, in
// Ownership.cpp) seeds `states_[lsym].source` via `borrowSourceForExpr` for
// every non-`using`, non-owned-marker initializer. `visitStmt`'s
// `LocalConstDecl` arm (same file) never seeded `states_` at all — its
// comment said so explicitly ("a const local carries no obligation ...
// treats the binding as a no-op"), conflating "no OWNERSHIP obligation"
// (correct: a const never disposes anything) with "no BORROW tracking"
// (wrong: §21.1 explicitly admits a const to hold a borrow). Every later
// consumer of a borrow source (`borrowSourceForExpr`'s own multi-hop-chain
// branch, `ownedLocalBorrowSource`, `checkBorrowStoreEscape`) reads
// `states_[sym].source`, which was simply never populated for a const
// symbol — a `states_.find(sym)` miss, not a wrong verdict, so the escape
// check silently never ran.
//
// THE FIX: `visitStmt`'s `LocalConstDecl` arm now calls
// `borrowSourceForExpr(*lc->value)` and, if it finds a source, records
// `states_[lsym] = {state: Borrowed, source: {src}}` — the SAME call
// `handleLocalDecl` already makes for a `var` initializer at the end of that
// function, gated on `!ld.isUsing && !ld.initializerHasOwnedMarker` (both
// unconditionally true for a `const`, which can never be `using` and never
// carries the `owned` transfer marker). No ownership STATE (disposal
// obligation) is recorded — only the borrow source — so a const still takes
// no obligation exactly as §21.1 requires.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

bool diagContains(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

bool diagContains(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The exact shape #3687 names: a `using var` scope-bound owned local
// re-borrowed by a bare `binding`, then stored into a field of a
// longer-lived object.
std::string fieldStoreProg(const std::string& binding) {
    return "module M; "
           "class Widget public V: int := 0; destructor end destructor; end Widget; "
           "class Holder public w: Widget?; end Holder; "
           "class C "
           "  public static method Main(): int "
           "    var h := new Holder(); "
           "    using var localObj := new Widget(); " +
           binding +
           "    h.w := cur; "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §28.3 "Dest::OuterLocal" escape shape: the re-borrow is stored into a
// local declared in a STRICTLY WIDER enclosing scope than the source.
std::string outerLocalStoreProg(const std::string& binding) {
    return "module M; "
           "class Widget public V: int := 0; destructor end destructor; end Widget; "
           "class C "
           "  public static method Main(): int "
           "    var prev: Widget? := null; "
           "    do "
           "      using var localObj := new Widget(); " +
           binding +
           "      prev := cur; "
           "    end do; "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// =============================================================================
// NEGATIVE — bare-identifier `const` re-borrow stored into a FIELD.
// FAILS WITHOUT THE FIX: compiles clean (hasErrors() is false); the program
// then reads through `h.w` after `localObj` is disposed at Main's scope end
// (measured under ASan: heap-use-after-free).
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           const_bare_id_field_store_rejected) {
    auto r = parseAndCheckOwnership(fieldStoreProg("const cur := localObj;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "would outlive its source"));
    BEZEL_EXPECT_TRUE(diagContains(r, "localObj"));
}

// =============================================================================
// CONTROL — the `var` twin was already rejected before this fix and must
// stay rejected, with the identical diagnostic text (this fix must not have
// touched the var path at all).
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           var_bare_id_field_store_rejected_control) {
    auto r = parseAndCheckOwnership(fieldStoreProg("var cur := localObj;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "would outlive its source"));
    BEZEL_EXPECT_TRUE(diagContains(r, "localObj"));
}

// =============================================================================
// NEGATIVE — the same const re-borrow stored into an OUTER-SCOPE local
// (§28.3's "Dest::OuterLocal" shape) — a second, independently-reached
// consumer of the same `states_[sym].source` seeding this fix adds.
// FAILS WITHOUT THE FIX: compiles clean.
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           const_bare_id_outer_local_store_rejected) {
    auto r =
        parseAndCheckOwnership(outerLocalStoreProg("const cur := localObj;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "would outlive its source"));
    BEZEL_EXPECT_TRUE(diagContains(r, "localObj"));
}

// =============================================================================
// CONTROL — the `var` twin of the outer-local shape, unaffected.
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           var_bare_id_outer_local_store_rejected_control) {
    auto r =
        parseAndCheckOwnership(outerLocalStoreProg("var cur := localObj;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "would outlive its source"));
    BEZEL_EXPECT_TRUE(diagContains(r, "localObj"));
}

// =============================================================================
// NEGATIVE — a two-hop const-to-const alias chain (`const cur := localObj;
// const cur2 := cur;`) must also be caught: `borrowSourceForExpr`'s
// multi-hop-chain branch reads `states_[cur].source`, which this fix is what
// populates in the first place.
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           const_to_const_multihop_field_store_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public V: int := 0; destructor end destructor; end Widget; "
        "class Holder public w: Widget?; end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    using var localObj := new Widget(); "
        "    const cur := localObj; "
        "    const cur2 := cur; "
        "    h.w := cur2; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "would outlive its source"));
    BEZEL_EXPECT_TRUE(diagContains(r, "localObj"));
}

// =============================================================================
// ACCEPT CONTROL — a const copying a VALUE-typed projection (`localObj.V`,
// an `int`) is a COPY, not a borrow (§28.3's borrow-vs-copy gate,
// `returnedExprMayBorrow`) — must never be rejected.
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           const_value_typed_projection_accepted_control) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public V: int := 0; destructor end destructor; end Widget; "
        "class Holder public v: int; end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    using var localObj := new Widget(); "
        "    const cur := localObj.V; "
        "    h.v := cur; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// ACCEPT CONTROL — a const re-borrowing a PARAMETER (not a scope-bound
// owned local) stored into a field: §28.3's residual-trust admission for
// parameter-sourced borrows. Parameters are never seeded into `states_`
// (they are owned-at-entry by declaration, not by dataflow), so
// `borrowSourceForExpr` finds no tracked source either before or after this
// fix — must stay accepted, exactly as the `var` twin already is.
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           const_parameter_sourced_borrow_field_store_accepted_control) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public V: int := 0; end Widget; "
        "class Holder public w: Widget?; end Holder; "
        "class C "
        "  public static method Set(h: Holder, p: Widget) "
        "    const cur := p; "
        "    h.w := cur; "
        "  end Set; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// ACCEPT CONTROL — an ordinary const initializer (no borrow of any tracked
// owned local anywhere) is completely unaffected.
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           ordinary_const_initializer_unaffected_control) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    const n := 5; "
        "    const m := n + 1; "
        "    return m; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// INTEGRATION — the real AOT pipeline (full driver: lex/parse/sema/ownership,
// the same path `bezelc` uses) rejects the exact #3687 repro, not just the
// isolated ownership-pass harness above. `r.ran` stays false on any
// front-end error, so a program that reaches this check never gets to
// codegen/link/execute — the use-after-free this fix closes is never
// compiled into a runnable binary at all.
// FAILS WITHOUT THE FIX: `r.diagnostics.hasErrors()` is false (the program
// would compile and, if run, read through freed memory — measured under
// ASan on the pre-fix tree: heap-use-after-free, `free` called from
// `localObj`'s scope-end disposal, READ from `Console.WriteLine(h.w!.V)`).
// =============================================================================
BEZEL_TEST(const_borrow_field_store_escape_3687,
           aot_pipeline_rejects_const_field_store_uaf) {
    auto r = runAot(
        "module M; import System; "
        "class Widget public V: int := 0; destructor end destructor; end Widget; "
        "class Holder public w: Widget?; end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    using var localObj := new Widget(); "
        "    const cur := localObj; "
        "    h.w := cur; "
        "    Console.WriteLine(h.w!.V); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "would outlive its source"));
    BEZEL_EXPECT_TRUE(diagContains(r, "localObj"));
}
