// Ownership — `visitStmt`'s `LocalConstDecl` arm never called `checkRead` on
// its initializer, so a `const` initializer that bare-reads an already
// moved-from or already-disposed owned local went completely undiagnosed —
// where the identical `var` spelling is correctly rejected/warned. Issue
// #3864.
//
// Spec, v0.294:
//   §21.1 "Variable declarations", the runtime-const "Ownership" paragraph,
//     verbatim: "...and **borrows** — the binding is then a borrow carrying
//     the initializer's source-liveness constraints, exactly as a plain
//     unmarked `var` alias: the consume-site freeze ([K5]) sees it as any
//     other live local borrow, and its constraint on the source ends at its
//     last use..."
//   §28.3 "Local variables, ownership, and borrows", K1: "3. moved-from —
//     the obligation has transferred out; the binding or slot is unreadable
//     until reassigned..." and "4. maybe-consumed..."; the glossary/K1.4
//     text also defines "explicitly-disposed" as falling under "§28.2's
//     use-after-dispose warning regime rather than the moved-from error".
//   §28.2 "The `IDisposable` contract" — the use-after-dispose diagnostic
//     regime a dominating explicit `Dispose()` call puts a later read under.
//
// THE BUG (measured pre-fix on HEAD 2760e31):
//
//   var x := new Res(); Sink.Take(move x); var   y := x;  → REJECTED:
//     "owned value in `x` was already moved out at this point (§28.3)"
//   var x := new Res(); Sink.Take(move x); const y := x;  → ACCEPTED, zero
//     diagnostics — `Sink.Take(owned r: Res)` auto-disposes the moved-in
//     value at method end (§28.3 auto-drop), so `y` aliases already-freed
//     memory; confirmed under ASan: heap-use-after-free on a later read of
//     `y`.
//
//   var x := new Res(); x.Dispose(); var   y := x;  → §28.2's existing
//     WARNING: "use of `x` after a dominating explicit Dispose() call..."
//   var x := new Res(); x.Dispose(); const y := x;  → ACCEPTED, zero
//     diagnostics (not even a warning).
//
// ROOT CAUSE: `MethodOwnership::handleLocalDecl` (the `var` path, in
// Ownership.cpp) calls `checkRead(*ld.initializer, /*selfTarget=*/false)`
// before classifying the new binding's state — the ONE function
// (`Ownership_Checks.cpp`) that detects a read of a local already in
// `MovedFrom`/`Disposed`/maybe-consumed/uninitialized state.  `visitStmt`'s
// `LocalConstDecl` arm (same file) never called it at all: its existing
// calls (`constInitializerRejected`/`producerState`, added by #2709; the
// `recordMovesIn` walk, added by #3591; the `borrowSourceForExpr` seed,
// added by #3687) each answer a DIFFERENT question — whether the WHOLE
// initializer is a rejected owned production, whether the initializer's own
// CALL sub-expressions write through owned state via `ref`/`out`, and
// whether the const should be tracked as a borrow SOURCE — none of them
// walk the initializer looking for a bare identifier read of a local
// already in an error state.
//
// THE FIX: the `LocalConstDecl` arm now calls
// `checkRead(*lc->value, /*selfTarget=*/false)` — the same authority
// `handleLocalDecl` consults, not a second copy — guarded by the SAME
// `topLevelConsume` condition `handleLocalDecl` uses (a top-level `move` or
// `await`).
//
// #3941 (review of the first #3864 landing): the guard is REQUIRED, not
// merely mirrored for symmetry. The original fix reasoned no guard was
// needed because §21.1 already rejects a top-level `move x`/`owned`-marker
// shape outright (no false ACCEPT is possible), which is true but
// incomplete: `recordMovesIn` (below) still runs unconditionally and, for a
// top-level `move x`, reaches `markMovedByName`'s precise `Disposed` branch
// ("cannot move from disposed local `x` (§28.3)", an ERROR) — but only if
// `x`'s state is STILL `Disposed` when `markMovedByName` looks. An
// unguarded `checkRead` sees `x` first, reports the generic §28.2 warning,
// and POISONS the state; `markMovedByName` has no `Poisoned` branch (by
// design) and silently falls through, so the precise ERROR never fires.
// Measured differentially against `origin/main` (pre-#3864,
// `git show 2760e31:src/ownership/Ownership.cpp`): `var x := new Res();
// x.Dispose(); const y := move x;` emits TWO diagnostics pre-#3864 (the
// §21.1 "must not take an owned production" error AND the precise
// "cannot move from disposed local `x` (§28.3)" error) — the unguarded
// #3864 fix silently downgraded the second to the generic §28.2 warning
// (still an accepted-as-rejected program overall, but a diagnostic-
// precision regression). The guard restores the exact pre-#3864 two-
// diagnostic output.
//
// per §21.1, §28.2, §28.3, v0.294, #3864, #3941

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

bool diagContainsWithSeverity(const OwnershipResult& r, bezel::Severity sev,
                              const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != sev) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// `Res` is a plain disposable class; `Sink.Take` takes ownership of its
// argument and never discharges it (return/`.Dispose()`/`move`), so the
// §28.3 auto-drop implicitly disposes the moved-in value at the end of
// `Take` — the moved-FROM local `x` back in `Main` is provably already
// disposed once `Take` returns. `binding` is substituted with either
// `const y := x;` or `var y := x;`.
std::string moveProg(const std::string& binding) {
    return "module M; class Res "
           "  public constructor() end constructor; "
           "  destructor end destructor; "
           "end Res; "
           "class Sink "
           "  public static method Take(owned r: Res) end Take; "
           "end Sink; "
           "class C "
           "  public static method Main(): int "
           "    var x := new Res(); "
           "    Sink.Take(move x); " +
           binding +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// Same shape, but the local is explicitly `.Dispose()`d rather than moved.
std::string disposeProg(const std::string& binding) {
    return "module M; class Res "
           "  public constructor() end constructor; "
           "  destructor end destructor; "
           "end Res; "
           "class C "
           "  public static method Main(): int "
           "    var x := new Res(); "
           "    x.Dispose(); " +
           binding +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// #3941: `x` is explicitly `.Dispose()`d, then TOP-LEVEL `move`d into the
// binding — no intervening `Sink.Take`, no bare-identifier read anywhere.
// `binding` is substituted with either `const y := move x;` or
// `var y := move x;`.
std::string disposeThenMoveProg(const std::string& binding) {
    return "module M; class Res "
           "  public constructor() end constructor; "
           "  destructor end destructor; "
           "end Res; "
           "class C "
           "  public static method Main(): int "
           "    var x := new Res(); "
           "    x.Dispose(); " +
           binding +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// =============================================================================
// NEGATIVE — a const initializer that bare-reads an already MOVED-FROM local
// must be rejected with the exact §28.3 "already moved out" text, matching
// the pre-existing var diagnostic.
// FAILS WITHOUT THE FIX: compiles clean (hasErrors() is false).
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           const_initializer_reads_moved_from_local_rejected_28_3) {
    auto r = parseAndCheckOwnership(moveProg("const y := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "owned value in `x` was already moved out at this point (§28.3)"));
}

// =============================================================================
// CONTROL — the var twin was already rejected before this fix and must stay
// rejected, with the IDENTICAL diagnostic text (proves the fix routes the
// const arm through the same authority, not a second drifting copy).
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           var_initializer_reads_moved_from_local_rejected_28_3_control) {
    auto r = parseAndCheckOwnership(moveProg("var y := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "owned value in `x` was already moved out at this point (§28.3)"));
}

// =============================================================================
// NEGATIVE — a const initializer that bare-reads an already explicitly-
// DISPOSED local must draw §28.2's existing use-after-dispose WARNING,
// matching the pre-existing var diagnostic (severity included — this is a
// warning, not an error, per the project's existing checked-disposal
// policy).
// FAILS WITHOUT THE FIX: zero diagnostics at all.
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           const_initializer_reads_disposed_local_warns_28_2) {
    auto r = parseAndCheckOwnership(disposeProg("const y := x;"));
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Warning,
        "use of `x` after a dominating explicit Dispose() call"));
    // A use-after-dispose is a WARNING, not an error (§28.2's existing
    // policy) — the const spelling must match the var spelling exactly, not
    // over-reject by promoting it to an error.
    BEZEL_EXPECT_FALSE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "use of `x` after a dominating explicit Dispose() call"));
}

// =============================================================================
// CONTROL — the var twin's existing warning, unchanged by this fix.
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           var_initializer_reads_disposed_local_warns_28_2_control) {
    auto r = parseAndCheckOwnership(disposeProg("var y := x;"));
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Warning,
        "use of `x` after a dominating explicit Dispose() call"));
}

// =============================================================================
// ACCEPT CONTROL — an ordinary const initializer bare-reading a LIVE
// (never moved, never disposed) local is a plain §21.1 borrow and must stay
// completely clean: the fix must not turn every const-of-identifier
// initializer into a false rejection.
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           const_initializer_reads_live_local_accepted_control) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    using var x := new Res(); "
        "    const y := x; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_FALSE(diagContains(r, "§28.2"));
}

// =============================================================================
// SIBLING COVERAGE — the same moved-from read inside a NESTED scope (an
// `if` body) reaches the same fix, since visitStmt's IfStmt arm recurses
// via visitStmt on each branch statement.
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           const_initializer_reads_moved_from_local_in_nested_scope_28_3) {
    auto r = parseAndCheckOwnership(moveProg(
        "if true then const y := x; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "owned value in `x` was already moved out at this point (§28.3)"));
}

// =============================================================================
// #3941 REGRESSION PIN — `const y := move x;` where `x` is already
// explicitly disposed is a TOP-LEVEL `move` initializer: §21.1 rejects the
// shape outright (an owned production), but `recordMovesIn`'s
// `markMovedByName` must ALSO still fire its own precise
// "cannot move from disposed local `x` (§28.3)" ERROR — the exact
// diagnostic `origin/main` (pre-#3864) produces. Asserts diagnostic TEXT
// and SEVERITY (Error), not `hasErrors()`, so a silent downgrade to the
// generic §28.2 WARNING (the #3941 regression) cannot pass unnoticed.
// FAILS WITHOUT THE #3941 FIX: the §21.1 rejection still fires (so
// `hasErrors()` stays true), but the precise §28.3 "cannot move from
// disposed local" ERROR is replaced by the generic §28.2 WARNING — this
// test's `diagContainsWithSeverity(..., Error, "cannot move from disposed
// local ...")` assertion is what catches that silently.
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           const_top_level_move_of_disposed_local_keeps_precise_28_3_error_3941) {
    auto r = parseAndCheckOwnership(disposeThenMoveProg("const y := move x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "a `const` local (`y`) must not take an owned production"));
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "cannot move from disposed local `x` (§28.3)"));
    // The precise error must NOT have been downgraded to the generic
    // use-after-dispose warning (the exact #3941 failure mode).
    BEZEL_EXPECT_FALSE(diagContainsWithSeverity(
        r, bezel::Severity::Warning,
        "use of `x` after a dominating explicit Dispose() call"));
}

// =============================================================================
// CONTROL — the var twin's pre-existing behavior (handleLocalDecl's own
// topLevelConsume guard), unaffected by this fix, confirming both binding
// forms now produce the identical precise §28.3 error.
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           var_top_level_move_of_disposed_local_keeps_precise_28_3_error_3941_control) {
    auto r = parseAndCheckOwnership(disposeThenMoveProg("var y := move x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "cannot move from disposed local `x` (§28.3)"));
}

// =============================================================================
// SIBLING COVERAGE — a const initialized from a CALL result whose ARGUMENT
// (not the whole initializer) is the moved-from read; checkRead's CallExpr
// arm recurses into each argument, so this reaches the fix too.
// =============================================================================
BEZEL_TEST(local_const_decl_checkread_3864,
           const_initializer_call_argument_reads_moved_from_local_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res) end Take; "
        "  public static method Peek(r: Res): int return 0; end Peek; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(); "
        "    Sink.Take(move x); "
        "    const n := Sink.Peek(x); "
        "    return n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "owned value in `x` was already moved out at this point (§28.3)"));
}
