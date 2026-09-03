// Spec-derived regression tests for issue #3790 — `markOutOwnedArgs` re-resolves
// the callee's formals through `resolveCalleeParams` without the chain
// (`this(...)` / `base(...)`) fallback that `recordMovesInArgs` uses. On a chain
// call the chain target's `out owned` parameter is invisible to the marking
// pass, so a chain recipient is silently unmarked.
//
// ── THE SHAPE (issue 3790's repro, verbatim) ───────────────────────────────
//
//   class Base
//     public constructor(out owned r: R) r := new R(); end constructor;
//   end Base;
//
//   class Derived: Base
//     public constructor(x: int)
//       base(out owned var recipient);   // §22.4 inline declaration
//     end constructor;
//   end Derived;
//
// ── WHAT THE DEFECT ACTUALLY WAS (measured on r12-cycle-1 @ 3f044f3) ───────
//
// `markOutOwnedArgs(args, callee)` (`Ownership_Checks.cpp` ~L8581) re-resolved
// the callee's formals through `resolveCalleeParams`, which returns an empty
// view for `this`/`base` (those are bare identifiers, not method symbols). The
// chain target's `out owned` parameter therefore never reached
// `markOutOwnedArgsCore`'s `boundModifier(args, pos, params) == "out owned"`
// check on a chain call — `params` was empty, `boundModifier` returned
// empty, `userOutOwned` was false, `markOwnedByName` /
// `preDeclaredOutOwnedRecipients_.insert(rsym)` never ran. The recipient
// stayed Borrowed: no §28.3 auto-drop warning, no `everOwnedPlainLocals_`
// entry, and — at runtime, for a compile-clean case — a leak against a value
// the callee had already transferred into its `owned` slot.
//
// `recordMovesInArgs` already routed through `calleeOrChainFormals`
// (the #3751 fix), and `collectCondOutOwnedRecipients` did the same (the
// #3176 fix). `markOutOwnedArgs` was the one remaining consumer reading
// `resolveCalleeParams` directly. The two callers of a constructor chain now
// agree about which formal an argument binds to.
//
// ── WHY THE OBSERVABLE SHAPE IS A §28.3 AUTO-DROP WARNING ──────────────────
//
// For an inline `out owned var recipient` at a chain call site, post-fix the
// recipient enters `states_` with state Owned (via the #2436 unconditional
// reseed at `markOutOwnedArgsCore`) and `everOwnedPlainLocals_` (so the
// end-of-method discharge check fires on it). Pre-fix neither half ran, so
// the dataflow sees no Owned obligation — no warning. The diagnostic
// presence/absence is the FAIL-WITHOUT / PASS-WITH signature.
//
// `hasErrors()` is not asserted: a downstream codegen ICE (#1386) on the same
// shape is unrelated and would mask the ownership fix's signature if folded
// in. Pin on the §28.3 auto-drop warning only, exactly as #3779's owned-IN
// twin pinned on the §22.3 marker demand only.
//
// ── ROWS THAT PASS ON BOTH SIDES, BY DESIGN ────────────────────────────────
//
// R3 / R4 are control rows: a plain (non-chain) `out owned` recipient still
// triggers the §28.3 auto-drop warning (R3), and a chained constructor
// WITHOUT an `out owned` formal produces no warning at all (R4). A fix that
// over-fired would draw R4's warning; a fix that under-fired would have
// dropped R3's.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// Minimal owning class — no counters, no destructor (we are not exercising
// runtime disposal here; the test pins the ownership-pass classification).
const char* kR =
    "class R "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end R; ";

// The §22.4 / §28.3 auto-drop warning text — matched by `hasAutoDropWarning`,
// scoped to a name. The pre-fix signature at a chain's `out owned` site is
// exactly its absence.
bool hasOutOwnedVarAutoDropWarning(const OwnershipResult& r,
                                   const std::string& name) {
    return hasAutoDropWarning(r, name);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// R1 — ACCEPT (with §28.3 warning). THE HEADLINE REPRO: a `base(out owned
// var recipient)` chain whose target is a constructor with an `out owned`
// parameter. Post-fix the recipient is marked Owned and the end-of-method
// discharge check fires the §28.3 auto-drop warning (because the recipient is
// not explicitly discharged). Pre-fix the recipient is silently unmarked —
// no warning at all — and the value is leaked at runtime.
// FAIL-WITHOUT: no §28.3 auto-drop warning naming `recipient`.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 (the `out owned` modifier) / §22.4 (inline `out owned var`) /
// §22.10 (constructor chaining) / §12.8 (the chain call's leading-statement
// position) / §28.3 (the auto-drop obligation), issue #3790
BEZEL_TEST(out_owned_chain_call_marked_3790,
           base_chain_out_owned_var_recipient_marked_owned) {
    auto r = parseAndCheckOwnership(
        std::string("module M3790a; ") + kR +
        "class Base "
        "  public constructor(out owned r: R) "
        "    r := new R(); "
        "  end constructor; "
        "end Base; "
        "class Derived: Base "
        "  public constructor(x: int) "
        "    base(out owned var recipient); "
        "  end constructor; "
        "end Derived; ");
    BEZEL_EXPECT_TRUE(hasOutOwnedVarAutoDropWarning(r, "recipient"));
}

// ═══════════════════════════════════════════════════════════════════════════
// R2 — ACCEPT (with §28.3 warning). The §12.8 SIBLING chain twin: a
// `this(...)` chain (within the same class) where the chain target has an
// `out owned` parameter. Same defect shape, different chain identifier — the
// chain fallback distinguishes `this` and `base` by NAME
// (`resolveChainCtorParams`, line ~518), so both arms of the fallback must
// be exercised.
// FAIL-WITHOUT: no §28.3 auto-drop warning naming `recipient`.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 / §22.4 / §22.10 / §12.8, issue #3790
BEZEL_TEST(out_owned_chain_call_marked_3790,
           this_chain_out_owned_var_recipient_marked_owned) {
    auto r = parseAndCheckOwnership(
        std::string("module M3790b; ") + kR +
        "class Box "
        "  public constructor(out owned r: R) "
        "    r := new R(); "
        "  end constructor; "
        "  public constructor(tag: int) "
        "    this(out owned var recipient); "
        "  end constructor; "
        "end Box; ");
    BEZEL_EXPECT_TRUE(hasOutOwnedVarAutoDropWarning(r, "recipient"));
}

// ═══════════════════════════════════════════════════════════════════════════
// R3 — CONTROL: a NON-CHAIN `out owned var recipient` site still fires the
// §28.3 auto-drop warning. This was working before the fix and must keep
// working; a fix that broke the ordinary (non-chain) path would draw no
// warning here. Anti-over-fire pin.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 / §22.4 / §28.3, issue #3790
BEZEL_TEST(out_owned_chain_call_marked_3790,
           non_chain_out_owned_var_recipient_still_marked_owned) {
    auto r = parseAndCheckOwnership(
        std::string("module M3790c; ") + kR +
        "class Factory "
        "  public method Make(out owned r: R) "
        "    r := new R(); "
        "  end Make; "
        "end Factory; "
        "class C "
        "  public static method Main(): int "
        "    using var f := new Factory(); "
        "    f.Make(out owned var x); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(hasOutOwnedVarAutoDropWarning(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// R4 — CONTROL: a chained constructor WITHOUT an `out owned` parameter
// produces no §28.3 auto-drop warning at all (the chain target's formals are
// all plain `out`, so the chain recipient is not an owned-OUT recipient;
// it's a borrow write that the callee writes through, no Owned obligation
// on the caller). Anti-over-fire pin: a fix that classified every chain
// recipient as `out owned` would draw R4's warning.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 / §22.10 / §28.3, issue #3790
BEZEL_TEST(out_owned_chain_call_marked_3790,
           chain_with_plain_out_recipient_no_owned_obligation) {
    auto r = parseAndCheckOwnership(
        std::string("module M3790d; ") + kR +
        "class Base "
        "  public constructor(out r: R) "
        "    r := new R(); "
        "  end constructor; "
        "end Base; "
        "class Derived: Base "
        "  public constructor(x: int) "
        "    base(out recipient); "
        "  end constructor; "
        "end Derived; ");
    // No §28.3 auto-drop warning for `recipient` — the chain target's formal
    // is plain `out`, not `out owned`, so no owned-OUT obligation. The
    // chain call is also a pre-declared local write, not an inline
    // declaration, so no `everOwnedPlainLocals_` entry fires either.
    BEZEL_EXPECT_FALSE(hasOutOwnedVarAutoDropWarning(r, "recipient"));
}
