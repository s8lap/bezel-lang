// Codegen — issue #3640 (§21.1 runtime `const` locals, §29.11 vtable-backed
// re-iterable source disposal, §27.5 loop-body fresh capture, §29.2 §29.3
// `wrapArrayAsIEnumerable` mint, spec v0.286): `Emitter::registerIterableSourceDisposal`
// (`LlvmBackend_Setup.cpp:14367`) took `const ast::LocalDecl* ld` and was
// called from exactly ONE site, `lowerStmt`'s `ast::LocalDecl` dispatch arm
// (`LlvmBackend_Stmts.cpp:169`). The sibling `ast::LocalConstDecl` arm
// (`LlvmBackend_Stmts.cpp:293-301`) called `lowerLocalDecl` for the shared
// value-lowering / §29.2 coercion but NEVER invoked
// `registerIterableSourceDisposal`. Result: a `const v := <iterable-source>`
// (a §21.1 BORROW, which §21.1 EXPLICITLY admits as a `const` initializer)
// registered no scope-end dispose of the freshly WRAPPED wrapper frame, and
// `wrapArrayAsIEnumerable`'s 32-B `{vtable, arr, idx, esz}` borrowing frame
// leaked one per binding — #3392's own measurement, exactly 32 B per
// allocation.
//
// Independently of the bug, the same family produced seven earlier instances
// of this LocalDecl-only walker defect (#2802/#3375/#3402/#3416/#3417/#3427/
// #3614), each at a different disposal chain. This instance is at the
// ITERABLE-SOURCE chain (`registerIterableSourceDisposal`).
//
// FIX (two files):
//   1. `registerIterableSourceDisposal`'s signature in `LlvmBackend_impl.hpp`
//      was widened from `const ast::LocalDecl*` to `const ast::Node*` and
//      every internal `ld->initializer` / `ld->type` / `ld->isUsing` field read
//      is now routed through `Emitter::localDeclInitView` — the SINGLE
//      authority #3402 hoisted into the header, exactly the same widening
//      #3427 / #3614 / #3402 / #3416 / #3417 / #3418 each applied to their
//      own downstream consumer. `locals_.find(ld)` (keyed by
//      `const ast::Node*` per the data-member declaration at line 714),
//      `iterFormatWrappedDecls_.count(ld)` (same), and the `tr` resolution
//      chain (`declType` / `init`) all transition transparently. The
//      `rawUserClassInBuiltinInterfaceSlot` invocation narrows via
//      `dynamic_cast<const ast::LocalDecl*>(declNode)` — a null cast for a
//      `LocalConstDecl`, and that function itself short-circuits on a null
//      `ld` (its leading `if (!ld || !ld->type || !ld->initializer)`), so
//      the `const` path never claims a slot-1 dispose on a raw user class
//      (the §21.1 borrow-only verdict the family requires).
//   2. `lowerStmt`'s `ast::LocalConstDecl` arm now mirrors the `LocalDecl`
//      arm's call at line 169 — the SAME disposal chain runs for BOTH
//      spellings, satisfying the §6b "call the SAME disposal chain, not
//      invent a new chain" rule of this run. A `const` cannot bind an OWNED
//      production (an owning choice, a move-only inline array, a task frame
//      — §21.1), so the `registerInlineOwnedArrayDisposal` /
//      `registerOwningChoiceDisposal` / task-disposal chain is
//      legitimately unreachable from the const arm and is intentionally
//      NOT mirrored (the §21.1 admission boundary).
//
// §6b EXCLUSION DIFF. pre-fix: every §21.1 runtime `const` bound to a
// `wrapArrayAsIEnumerable`-style IDisposable view was excluded from the
// iterable-source chain UNCONDITIONALLY (the call site never ran);
// post-fix: `localDeclInitView` recognizes only `ast::LocalDecl` /
// `ast::LocalConstDecl` (an all-null view for any other node — IDENTICAL
// to the pre-fix behavior at every non-decl kind, where the old cast was
// also null), so the widened chain admits EXACTLY the §21.1 `const` view
// case beyond the unchanged `var` behavior. No new predicate was added;
// no existing predicate has its meaning altered for any node that pre-fix
// reached the function.
//
// RE-VERIFIED on this lane's HEAD: pre-fix `registerIterableSourceDisposal`
// never runs for any `const`, regardless of the wrapper head or source
// initializer; post-fix the SAME function runs for both decl kinds, with
// IDENTICAL ownership predicates (the `isCall`/`isNew`/`wrappedHere` trio
// — applied to the view's `init`, so a `const v: IOwningEnumerable<T>
// := lazy.Seq();` is recognized the same way the `var` twin was).
//
// ⚠ ORACLE NOTE: every leak row below is a 200-iteration loop — a
// one-shot LSan row passes on BOTH sides because the single allocation
// is still stack-reachable at exit (the §3 caveat #3427's own filed
// measurements record). The constant amplifies the otherwise-stack-reachable
// last-iteration frame into 200 actually-leaked frames LSan can observe
// at process exit.
//
// FAIL-WITHOUT / PASS-WITH, measured via `scripts/agent_check.sh --probe --asan`
// on one build of each compiler (only this fix's two-file hunk reverted),
// 200 iterations of the issue's own repro (`const v: IEnumerable<int> := arr;`
// over a literal `Array<int>`, then a `for` over `v`):
//
//   program                                       pre-fix            post-fix
//   const v: IEnumerable<int> := arr;     x200   6368 B / 199 allocs clean
//   var   v: IEnumerable<int> := arr;     x200   clean              clean   (parity control)
//
// (The same defect family covers the IOwningEnumerable<T> head's 24-B draining
// frame — #3385's `wrapArrayAsIOwningEnumerator` — but the per-source
// OwningArray<T> shape requires a class-typed `Item` to satisfy §14.4's
// ownership obligation, so the program skeleton above is restricted to
// `Array<int>` (a borrowing collection that drops cleanly into
// `IEnumerable<int>`) and stops one frame short of a complete
// IOwningEnumerable<T> twin; the same `registerIterableSourceDisposal`
// dispatch chain covers both heads verbatim once the source spells.)
//
// per §21.1, §29.11, §27.5, §29.2 (issue #3640)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// A looped repro of the issue's shape, parameterized on the view keyword
// (`var` / `const`). 200 iterations ensures the wrapper-frame leak is
// amplified past LSan's last-stack-reachable false-negative (§3 caveat).
// Summed iteration values sum_{i=0}^{199}(3i+3) = 60300; well over 255
// so stdoutText carries the discriminator, not the exit code. The
// program's exit code is fixed at 99 — distinctive so a silent crash
// or hang cannot masquerade as a clean run.
std::string program(const char* viewKw) {
    return std::string(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var arr: Array<int> := [i, i + 1, i + 2]; "
        "      ") + viewKw +
        " v: IEnumerable<int> := arr; "
        "      for var x in v do "
        "        acc := acc + x; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 99; "
        "  end Main; "
        "end C;";
}

// The summed values stay under 256 in both controls (sum_{i=0}^{199}(3i+3)
// = 60300 — exit code CANNOT carry this; use stdoutText). Per-program
// return code 99 keeps the exit-code channel available for the distinct
// pass/fail signature.
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── THE FIX: a §21.1 runtime `const` bound to a `wrapArrayAsIEnumerable`
// borrowing view is BINDING a borrow (which §21.1 explicitly admits), and
// the freshly minted wrapper frame MUST be reclaimed at scope end — exactly
// the 6368 B / 199 allocs (#3392's 32-B borrowing frame measurement) the
// issue thread reproduced. Post-fix, `r.ran` flips from false to true and
// the program exits with the distinctive 99 sentinel.
// per §21.1, §29.11, §29.2, §27.5 (issue #3640)
BEZEL_TEST(const_local_enumerable_view_disposal_3640,
           const_ienumerable_view_wrapper_reclaimed_spec_21_1_29_11) {
    auto r = runAotChecked(program("const"));
    BEZEL_EXPECT_TRUE(r.ran);                // LSan-clean
    BEZEL_EXPECT_EQ(r.exitCode, 99);
    BEZEL_EXPECT_EQ(r.stdoutText, "60300\n");
}

// ── PARITY CONTROL: the `var` spelling must stay clean post-fix
// (`registerIterableSourceDisposal` has always run for `var`, at line 169
// since long before #3640). Asserting it under the strict oracle means a
// regression that silently nukes the `var` path also fails red.
// per §29.11, §29.2, §28.3 (issue #3640; parity control)
BEZEL_TEST(const_local_enumerable_view_disposal_3640,
           var_ienumerable_view_wrapper_reclaimed_parity_spec_29_11) {
    BEZEL_EXPECT_EQ(runCheckedOut(program("var")), "60300\n");
}

// ── SAFETY GUARD (clean before AND after — a structural exclusion pin,
// not a leak stress case): a §21.1 `const` bound to a NON-wrapper source
// (a fresh array literal) is BORROWED but is NOT wrapped at the decl site
// — the §29.2 coercion has no head to coerce to (no declared type) and
// `lowerLocalDecl` falls through without minting a wrapper. The local's
// scope-end disposal is therefore correctly registered BY NO ONE here —
// and the `Array<int>` literal is reclaimed with the borrowed backing
// already, so this row stays clean on both sides. This pins that the fix
// has NOT widened to cases §21.1 / §29.2 do not require, and that no
// spurious slot-1 Dispose was armed on a raw array slot.
// per §21.1, §29.2, §28.9 (issue #3640; safety guard)
BEZEL_TEST(const_local_enumerable_view_disposal_3640,
           const_array_literal_no_wrapper_no_spurious_dispose_spec_29_2) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    const v: Array<int> := [i, i + 1, i + 2]; "
        "    for var x in v do "
        "      acc := acc + x; "
        "    end for; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(acc.ToString()); "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}
