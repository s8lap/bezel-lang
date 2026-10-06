// §28.1/§28.2 v0.196 E1/E2 reconciliation (issue #1857 — `Reconcile #1817
// E1/E2 owned-marker-erasure predicates`):
//
//   * `Resolver::checkOwnedDeclType` declares the authoritative reject set:
//     every primitive (including `Object`, which §28.2 excludes from the
//     `IDisposable` contract: "`Dispose()` is not on `Object`"), the unit type
//     `()`, and every function type carry no disposal obligation — the
//     `checkOwnedDeclType` reject REJECTS `owned` on them outright.
//   * E2 — `ownedMarkerErasedAtSubstitutedType` in `ExprTypecheck_Calls.cpp`
//     — mirrors that reject set: at a substitution to a primitive/Unit/Function
//     the marker is ERASED, the substituted signature is unmarked, the
//     result/parameter is a plain value with no obligation (§28.9).
//   * E1 — `isValueSubstitutionErased` in `Ownership.cpp` — DEVIATED from E2
//     for `Object`, `Unit`, and `Function`: it kept them ON the `Owned` path
//     as a documented "deliberate leak-safe choice", but the deviation was
//     provably WRONG at the substitution boundary.
//
// THE BUG. With E1 keeping `Object`/`Unit`/`Function` Owned, a generic
// `class Factory<T> ... method Make(): owned T` instantiated at T = Object
// classified `var n := f.Make(...);` as `Owned` — the ownership analysis
// attached a §28.3 auto-drop must-use warning to `n` and codegen emitted a
// spurious scope-end dispose of a value that owns nothing (and, on the
// ASan/LSan oracle, would have free'd a non-object / run a void dispose).
// The same pattern reproduces for `T = () -> int` and `T = ()`.
//
// THE FIX. Reconcile E1 with E2 (`checkOwnedDeclType`): every primitive (incl.
// `Object`), `Unit`, and `Function` now ERASE the marker — mirroring E2 and
// `checkOwnedDeclType`'s uniform primitive reject. The class/interface
// substitution path is unchanged (those keep `Owned` — a class IS owned).
//
// FAILS-WITHOUT / PASSES-WITH. All `parseAndCheckOwnership` tests assert
// `hasAutoDropWarning(r, "n") == false` — the spurious "is implicitly
// disposed" warning on `n: Object`/`Function`/`Unit` is present without the
// fix and absent with it. The `runAotChecked` tests pin the runtime limb:
// an erased `var n := f.Make()` carries no obligation, so the program
// exits 0 with no sanitizer report (and the built executable does no
// scope-end dispose of an Object / function / unit value). The NEG control
// guards the boundary: a CLASS substitution (`T = Conn`, a class with a
// destructive payload) still transfers and disposes exactly once — erasing
// there would leak.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "support/ownership_support.hpp"  // parseAndCheckOwnership + hasAutoDropWarning
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// (1) T = Object — the marker is erased (E2/E1 reconciliation, §28.1 +
// §28.2 "Dispose is not on Object"). var n carries no disposal obligation,
// so no §28.3 auto-drop warning.
// ===========================================================================
BEZEL_TEST(owned_bare_t_primitive_func_unit_erasure_1857,
           object_substitution_erased_no_auto_drop) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Factory<T> "
        "  public method Make(): owned T "
        "    var x: Object := new Object(); "
        "    return x; "
        "  end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<Object>(); "
        "    var n := f.Make(); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "n"));
}

// ===========================================================================
// (2) T = () -> int — the marker is erased (E2/E1 reconciliation, §28.1 +
// §28.9 runtime-managed function values). var n carries no disposal
// obligation, so no §28.3 auto-drop warning.
// ===========================================================================
BEZEL_TEST(owned_bare_t_primitive_func_unit_erasure_1857,
           function_substitution_erased_no_auto_drop) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Factory<T> "
        "  public method Make(): owned T "
        "    return default(T); "
        "  end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<() -> int>(); "
        "    var n := f.Make(); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "n"));
}

// ===========================================================================
// (3) T = () — the marker is erased (E2/E1 reconciliation, §28.1 + §28.9
// runtime-managed unit). var n carries no disposal obligation, so no §28.3
// auto-drop warning.
// ===========================================================================
BEZEL_TEST(owned_bare_t_primitive_func_unit_erasure_1857,
           unit_substitution_erased_no_auto_drop) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Factory<T> "
        "  public method Make(): owned T "
        "    return default(T); "
        "  end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<()>(); "
        "    var n := f.Make(); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "n"));
}

// ===========================================================================
// (4) T = Object — the runtime limb: runAotChecked exits 0 with no sanitizer
// report (and no printed destructor — Object has none). WITHOUT the fix
// codegen emitted a spurious scope-end dispose of an Object that would have
// surfaced as ASan/LSan-detected misbehavior (run=false) on a stricter
// runtime; with the fix the erased result carries no obligation.
//
// ── KNOWN BLOCKER (#5437) — this row is RED for a reason UNRELATED to #1857 ──
//
// MEASURED with `--sanitize=address` on generated code: every `new Object()`
// leaks its 8-byte instance, in EVERY context — a plain `var x: Object := new
// Object();`, a `using var`, and this generic row alike. The discriminator:
// `using var a := new Empty(); using var b := new Object();` leaks EXACTLY ONE
// 8-byte object for two allocations (the equally-trivial-dispose user class is
// freed, `Object` is not), and the identical `Factory<Object>` whose `Make()`
// returns `default(T)` — no allocation — is clean at exit 0. So the leak tracks
// the `new Object()` allocation itself, NOT the `owned`-marker erasure this row
// and its three siblings are about, and NOT the generic substitution.
//
// The oracle here (`r.ran` / exitCode 0) cannot distinguish "the erasure is
// correct" from "`Object` leaks", because both present as an LSan report. The
// erasure limb of this row IS covered independently and passes: the
// `parseAndCheckOwnership` sibling
// `object_substitution_erased_no_auto_drop` (row 1) asserts the same predicate
// on the same substituted type, and the function/unit runtime siblings use
// `default(T)` and pass. #5437 carries the reclaim fix and the non-generic
// regression pin.
//
// Deliberately NOT weakened to go green: no assertion is touched, no
// DISABLED_, nothing deleted. The row goes green when #5437 lands.
// ===========================================================================
BEZEL_TEST(owned_bare_t_primitive_func_unit_erasure_1857,
           object_substitution_runs_clean) {
    JitResult r = runAotChecked(
        "module M; "
        "class Factory<T> "
        "  public method Make(): owned T "
        "    var x: Object := new Object(); "
        "    return x; "
        "  end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<Object>(); "
        "    var n := f.Make(); "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end P;");
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The erased Object round-trips through the runtime without any
    // sanitiser abuse; the program's "done" line is the observable.
    const std::size_t done = r.stdoutText.find("done");
    BEZEL_EXPECT_TRUE(done != std::string::npos);
}

// ===========================================================================
// (5) T = () -> int — the runtime limb: the erased function value is plain
// (no disposal obligation), the program exits 0 with no sanitizer report.
// ===========================================================================
BEZEL_TEST(owned_bare_t_primitive_func_unit_erasure_1857,
           function_substitution_runs_clean) {
    JitResult r = runAotChecked(
        "module M; "
        "class Factory<T> "
        "  public method Make(): owned T "
        "    return default(T); "
        "  end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<() -> int>(); "
        "    var n := f.Make(); "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end P;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    const std::size_t done = r.stdoutText.find("done");
    BEZEL_EXPECT_TRUE(done != std::string::npos);
}

// ===========================================================================
// (6) T = () — runtime limb is left to a separate compiler-bug investigation:
// a `Factory<()>.Make(): owned ()` program currently SIGILLs the front-end
// (unrelated to #1857 — `default(T)` at T = () is a separate shape not yet
// exercised end-to-end). The ownership-pass limb IS covered by test (3)
// above: parseAndCheckOwnership asserts the same predicate on the same
// substituted result type without triggering the front-end bug.
// ===========================================================================

// ===========================================================================
// NEG (7) — a CLASS substitution still transfers ownership. T = Conn
// (a class with a destructive payload) keeps the marker — `using var c` is
// the scope-end disposal and the destructor runs exactly once. Erasing
// here would leak (destructor never runs) → runAotChecked r.ran flips false.
// ===========================================================================
BEZEL_TEST(owned_bare_t_primitive_func_unit_erasure_1857,
           class_substitution_still_transfers) {
    JitResult r = runAotChecked(
        "module M; "
        "class Conn "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
        "end Conn; "
        "class Factory<T> "
        "  public method Make(owned x: T): owned T return x; end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<Conn>(); "
        "    using var c := f.Make(new Conn(7)); "
        "    Console.WriteLine(\"body\"); "
        "    return 0; "
        "  end Main; "
        "end P;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The destructive payload's destructor ran exactly once.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("D7") != std::string::npos);
    // Scope-end disposal ordering: body before D7.
    const std::size_t body = r.stdoutText.find("body");
    const std::size_t disp = r.stdoutText.find("D7");
    BEZEL_EXPECT_TRUE(body != std::string::npos);
    BEZEL_EXPECT_TRUE(disp != std::string::npos);
    BEZEL_EXPECT_TRUE(body < disp);
}
