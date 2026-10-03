// Spec-derived tests for issue #5284 — a local bound to a BUILTIN STATIC
// `owned`-returning factory is classified as a BORROW, so `Take(move s)` into
// an `owned` parameter is rejected with the §28.8 "a borrow cannot be passed
// where an `owned` value is required" diagnostic.
//
// THE DEFECT. §28.8 lists the legal owned producers, and "a call returning
// `owned T`" is one of them. stdlib §8.1 declares, verbatim:
//
//   public class Stopwatch
//     public constructor();
//     public static method StartNew(): owned Stopwatch;
//
// so `Stopwatch.StartNew()` IS an owned producer and `var s :=
// Stopwatch.StartNew(); Take(move s);` is a legal §28.8 transfer. Pre-fix it
// was rejected. The cause is that a builtin static factory has NO
// `ast::MethodDecl` and NO `MethodSymbol`, so every `producerState` arm keyed
// on a callee signature is structurally unreachable for it:
//   * `md->returnOwned` — no MethodDecl to read (the §28.3 user-static path);
//   * `staticFactoryReturnsFreshOwned` — same, it takes a `MethodDecl`;
//   * `importedReturnOwned` (#1415) — populated only for imported module
//     members, and this is an always-available builtin, not a module member;
//   * `isBuiltinOwnedTransferCall` (#1388) — builtin owned-TRANSFER calls
//     (Pop/Dequeue/TakeCurrent), a different fact.
// The call fell through to the Borrowed default, `states_` recorded the local
// Borrowed, and the shared §28.8 owned-source classifier
// (`MethodOwnership::ownedArgSourceIsBorrow`) — which every owned-sink check
// consults — reported a borrow.
//
// THE FIX, and why it is at the CHECK layer and not in `producerState`. This
// mirrors the resolution of #4042 exactly, for the same reason: touching
// `producerState` would change DISPOSAL, and `producerState` is the
// free-deciding input at `handleLocalDecl`. The fix instead widens the ONE
// shared, side-effect-free fact the check layer already consults —
// `MethodOwnership::isMaterializerCarveOutBorrow`, extended to also recognize
// a local bound directly to a builtin static `owned` factory via the new
// `common::isBuiltinOwnedStaticFactoryCall`. `ownedArgSourceIsBorrow` has no
// side effects (every caller only feeds a `report(...)`), so the change can
// only make MORE programs compile; it cannot alter what codegen frees for any
// already-accepted program.
//
// WHY ADMITTING THE TRANSFER CANNOT DOUBLE-FREE (measured, not argued). #4042
// had to argue that codegen's escape walk already excludes the local from its
// own reclaim. For THIS family the argument is stronger and purely empirical:
// on the base commit r23-c28 (4e996839), generated-code ASan/LSan measured
// 299/299 leaked allocations for an otherwise-unused 300-iteration loop over
// `var s := Stopwatch.StartNew();`, 299/299 over `var c :=
// CancellationTokenSource.WithCancelAfter(…);`, 1499 over `var a :=
// File.ReadAllLines(…);` and 18300 over `var a := Directory.GetFiles(…);` —
// codegen registers NO scope-end reclaim for these locals at all (only the
// `using var` spelling reaches the disposal arms). So on the transfer path the
// value has exactly ONE disposer: the `owned` sink it is moved into. The
// `amplified_*` tests below pin that: they run the transfer in a loop under
// `runAotChecked` (which is the AOT + ASan oracle) and would report a
// double-free or a leak immediately.
//
// THE RESIDUAL, DELIBERATELY NOT FIXED HERE. A local from one of these
// factories that is never transferred anywhere still leaks (the measurement
// above IS that leak). Fixing it means widening `producerState` to `Owned`,
// which changes disposal for the 26 test files that use
// `Stopwatch.StartNew()` — a separate, larger change with its own
// FAIL-WITHOUT surface. The classification bug and the reclaim gap are
// distinct defects; this lane takes the one that rejects legal programs.
//
// NOT COVERED HERE, AND WHY. Two shapes in the same stdlib family are NOT
// claimed by the fix, because sema produces NO result TypeRef for them at
// all, so the predicate's result-head cross-check cannot (and must not) fire:
//   * `Directory.GetFiles` / `Directory.GetDirectories` (stdlib §7.4) — sema's
//     `colName == "Directory"` arm types only `Exists`; codegen lowers the two
//     but the call is left untyped, so `ctx.exprType(call)` is null (MEASURED
//     with temporary instrumentation: the factory table matched, the result
//     TypeRef did not exist). The predicate fails open there, which is the
//     correct polarity: claiming ownership of a value the front end never
//     typed would be unsound.
//   * `Environment.Variables(): owned Dictionary<string, string>` (stdlib §3)
//     — the same shape: no `Variables` arm anywhere in `src/`, and the call
//     degrades to a zero value (the #2434 diagnostic). Pre-fix its local was
//     rejected the same way; it still is. That is a distinct missing-sema-typing
//     defect, not a classification one.
//
// `FrozenArray<T>.From` is covered, but only in its EXPLICIT-type-argument
// spelling (`FrozenArray<int>.From(src)`). Sema's #3249 arm reads the type
// argument off the callee target IdentifierExpr's own `typeArgs`
// (ExprTypecheck_Calls.cpp), so the bare `FrozenArray.From(src)` spelling types
// nothing and is left alone for the same reason as `Directory.GetFiles`. The
// test below pins the spelling that is typed.
//
// The narrowness gates (each pinned by a control test below): a user class may
// take a builtin's spelling (§2.3 v0.313 — only the §3.4 reserved row is
// unshadowable), and such a class's own `static method StartNew(): owned
// Stopwatch` must keep classifying through the ordinary user-MethodDecl path.
// The predicate's `isUserDeclaredTypeSym` gate (§2.3 / #5272) is what keeps
// the two apart; a control test pins the user class still works, and a
// `reject_control` pins that a genuine borrow is still rejected.
//
// FAIL-WITHOUT / PASS-WITH: measured on this worktree by reverting the four
// source files to `r23-c28` and rebuilding — all eight accept/amplified rows
// below failed pre-fix with `assertion failed: jr.ran` (the compile was
// rejected with the §28.8 diagnostic text asserted in the `reject_control`
// needle constant), and ran clean (exit code as asserted) post-fix. The
// result was 6 passed, 8 failed pre-fix against 14 passed, 0 failed
// post-fix: the two `reject_control` tests and the four `control_*` tests
// compiled clean BOTH before and after, which is what makes them controls
// rather than pins.
//
// per §28.8, §28.3 *Producers*, §2.3, §2.7, stdlib §4.19 / §6.1 / §7.3 /
// §7.4 / §8.1, v0.313, #5284

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked — AOT(+ASan) oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kOwnedArgBorrow =
    "a borrow cannot be passed where an `owned` value is required";

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO — `var s := Stopwatch.StartNew(); Take(move s);`
// (§28.8; stdlib §8.1 `public static method StartNew(): owned Stopwatch;`).
// ===========================================================================

BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           stopwatch_startnew_via_local_move_to_owned_param_28_8_stdlib_8_1) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Take(owned s: Stopwatch) "
        "    s.Start(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var s := Stopwatch.StartNew(); "
        "    Take(move s); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// The import-ALIAS spelling (`import System.Diagnostics as D;
// D.Stopwatch.StartNew()`), which the issue's #5242 reviewer first saw. The
// receiver's class name sits in the last component of a dotted receiver here,
// not in a bare IdentifierExpr, so it is a distinct spelling of the same
// fact — the #4698 alias trap. MEASURED rejected pre-fix too.
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           stopwatch_startnew_via_import_alias_move_to_owned_param_28_8) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.Diagnostics as D; "
        "class P "
        "  public static method Take(owned s: D.Stopwatch) "
        "    s.Start(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var s := D.Stopwatch.StartNew(); "
        "    Take(move s); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// ===========================================================================
// THE SAME DEFECT ACROSS THE OTHER BUILTIN `owned` STATIC FACTORIES. Each of
// these was MEASURED rejected with the §28.8 needle pre-fix, on the base
// commit, in the same shape as the Stopwatch repro. They are the issue's
// "audit the other builtin owned factories" requirement: the fix subsumes all
// of them because they share ONE classification defect, not one per factory.
// ===========================================================================

// stdlib §6.1 — `WithCancelAfter(delay: TimeSpan): owned
// CancellationTokenSource` (also §30.8). The #463/#810 family.
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           cts_withcancelafter_via_local_move_to_owned_param_28_8_stdlib_6_1) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method Take(owned c: CancellationTokenSource) "
        "    c.Cancel(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var c := CancellationTokenSource.WithCancelAfter("
        "        TimeSpan.FromSeconds(30)); "
        "    Take(move c); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// stdlib §6.1 / §30.8 — `CreateLinked(a, b): owned CancellationTokenSource`,
// the TWO-argument sibling of the above (#810).
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           cts_createlinked_via_local_move_to_owned_param_28_8_stdlib_6_1) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method Take(owned c: CancellationTokenSource) "
        "    c.Cancel(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var c := CancellationTokenSource.CreateLinked("
        "        CancellationToken.None, CancellationToken.None); "
        "    Take(move c); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// stdlib §7.3 — `ReadAllLines(path: string): owned Array<string>`. The
// declaration-less `Array<T>` RESULT shape (no `builtinClassSym` declaration,
// so a different arm of the predicate's result cross-check).
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           file_readalllines_via_local_move_to_owned_param_28_8_stdlib_7_3) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.IO; "
        "class P "
        "  public static method Take(owned a: Array<string>) "
        "  end Take; "
        "  public static method Main(): int "
        "    var a := File.ReadAllLines(\"/etc/hostname\"); "
        "    Take(move a); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// stdlib §4.19 — `From(items: Array<T>): owned FrozenArray<T>`, in the
// EXPLICIT-type-argument spelling (the one sema's #3249 arm types; see the
// header comment on why the bare spelling is out of scope).
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           frozenarray_from_via_local_move_to_owned_param_28_8_stdlib_4_19) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.Collections.Immutable; "
        "class P "
        "  public static method Take(owned f: FrozenArray<int>) "
        "  end Take; "
        "  public static method Main(): int "
        "    using var src := new Array<int>(); "
        "    src.Add(1); src.Add(2); src.Add(3); "
        "    var f := FrozenArray<int>.From(src); "
        "    Take(move f); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// ===========================================================================
// AMPLIFIED RUNTIME PROOF (300 iterations, AOT+ASan oracle). One-shot runs are
// clean on both sides of a bug in this family, so the loop is what turns a
// per-iteration double-free or leak into an unmistakable ASan/LSan report.
// The value has exactly ONE disposer on this path (the `owned` sink), so these
// are clean by construction only if the transfer is classified correctly AND
// codegen registers no second free — which is the property under test.
// ===========================================================================

BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           amplified_stopwatch_startnew_transfer_no_leak_no_double_free_28_8) {
    std::string src =
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Take(owned s: Stopwatch) "
        "    s.Start(); s.Stop(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 300 do "
        "      var s := Stopwatch.StartNew(); "
        "      Take(move s); "
        "      i := i + 1; "
        "    end; "
        "    return 7; "
        "  end Main; "
        "end P;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           amplified_cts_withcancelafter_transfer_no_leak_no_double_free_28_8) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method Take(owned c: CancellationTokenSource) "
        "    c.Cancel(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 300 do "
        "      var c := CancellationTokenSource.WithCancelAfter("
        "          TimeSpan.FromSeconds(30)); "
        "      Take(move c); "
        "      i := i + 1; "
        "    end; "
        "    return 7; "
        "  end Main; "
        "end P;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// ===========================================================================
// CONTROLS — MUST STAY GREEN. Each of these compiled clean BOTH pre-fix and
// post-fix; that is what makes them controls rather than pins. They are the
// regression fence for the narrowness of the new predicate.
// ===========================================================================

// `new Stopwatch()` — the NewExpr producer arm, a SIBLING path this fix must
// not disturb (stdlib §8.1's `public constructor();`).
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           control_new_stopwatch_move_to_owned_param_still_accepted_28_8) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Take(owned s: Stopwatch) "
        "    s.Start(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var s := new Stopwatch(); "
        "    Take(move s); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// A USER class's own `static method Make(): owned W` (the issue's stated
// control) — the `md->returnOwned` path, untouched by this fix.
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           control_user_static_owned_factory_still_accepted_28_8) {
    JitResult jr = runAotChecked(
        "module M; "
        "class W "
        "  public constructor() end constructor; "
        "  public static method Make(): owned W "
        "    return new W(); "
        "  end Make; "
        "end W; "
        "class F "
        "  public static method Take(owned w: W) "
        "  end Take; "
        "end F; "
        "class P "
        "  public static method Main(): int "
        "    var w := W.Make(); "
        "    F.Take(move w); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// §2.3 (v0.313) SHADOWING: a user class may take a builtin's spelling, and its
// OWN `static method StartNew(): owned Stopwatch` must keep classifying
// through the ordinary user-MethodDecl path (`md->returnOwned`) — NOT through
// the new builtin predicate. This is the narrowness gate
// (`isUserDeclaredTypeSym`) pinned: without it, the predicate would claim a
// user type's factory as a builtin's.
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           control_user_class_shadowing_builtin_name_still_accepted_2_3) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.Diagnostics; "
        "class Stopwatch "
        "  public V: int := 3; "
        "  public static method StartNew(): owned Stopwatch "
        "    return new Stopwatch(); "
        "  end StartNew; "
        "end Stopwatch; "
        "class P "
        "  public static method Take(owned s: Stopwatch) "
        "  end Take; "
        "  public static method Main(): int "
        "    var s := Stopwatch.StartNew(); "
        "    Take(move s); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// The `using var` spelling of the same factory — codegen's disposal-arm path
// (LlvmBackend_Setup's Stopwatch registration), untouched by this fix.
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           control_using_var_startnew_still_accepted_28_3) {
    JitResult jr = runAotChecked(
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Main(): int "
        "    using var s := Stopwatch.StartNew(); "
        "    s.Start(); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// ===========================================================================
// REJECT CONTROLS — the §28.8 guard must still FIRE on a genuine borrow. The
// new predicate is a carve-out for one specific initializer SHAPE; a plain
// borrowed local, and a borrowed non-`owned` parameter, are untouched and must
// still be rejected. Compiled clean (rejected) BOTH pre- and post-fix.
// ===========================================================================

BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           reject_control_non_owned_param_move_still_rejected_28_8) {
    JitResult jr = runAot(
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Take(owned s: Stopwatch) "
        "  end Take; "
        "  public static method Borrow(s: Stopwatch): int "
        "    Take(move s); "
        "    return 1; "
        "  end Borrow; "
        "  public static method Main(): int "
        "    return Borrow(Stopwatch.StartNew()); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kOwnedArgBorrow));
}

// A local bound from a FURTHER ALIAS of the factory result (`var t := s;`) is
// not a DIRECT initializer, exactly as #4042's `transitive_alias` carve-out
// boundary — it keeps the conservative Borrowed classification and is still
// rejected. This is the "DIRECT initializer only" narrowness of the predicate.
BEZEL_TEST(builtin_static_owned_factory_local_sink_5284,
           reject_control_alias_of_factory_local_still_rejected_28_8) {
    JitResult jr = runAot(
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Take(owned s: Stopwatch) "
        "  end Take; "
        "  public static method Main(): int "
        "    var s := Stopwatch.StartNew(); "
        "    var alias := s; "
        "    Take(move alias); "
        "    return 7; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kOwnedArgBorrow));
}
