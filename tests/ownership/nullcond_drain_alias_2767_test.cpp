// fix #2767 / §21.1 / §29.11 (spec v0.240), stdlib §4.5 (v0.195): the
// null-conditional call spelling `recv?.Drain()` bypasses the
// `common::isOwningCollectionDrainAliasCall` predicate's callee gate, so the
// §21.1 `const` rejection (the §29.11 deferred-activation producer row) and
// every downstream consumer of the predicate — codegen's
// registerUsingDisposal / registerIterableSourceDisposal (#2146's bz_array
// double-dispose SEGV site) and ownership::MethodOwnership's producerState
// Borrowed carve / usingVarSourceIsBorrow — see nothing for the `?.` form.
//
// Spec, v0.240:
//   §21.1 "Variable declarations", Ownership paragraph: "Rejected, with a
//     diagnostic naming this rule and the `using var` remedy: a fresh `new`,
//     an `owned`-returning call, a `Task`-family or iterable-family call
//     (`Drain()` and `ReceiveAll()` included — ordinary minting productions,
//     §28.3); a lazy query likewise, its §29.11 single-consumption discipline
//     riding the minted frame".
//   §28.3 producers: "Drain() / ReceiveAll() — on `OwningArray<T>` and the
//     other owning collections — returns the collection itself at that static
//     type — no separate enumerable object" (stdlib §4.5 v0.195). The view
//     ALIASES the receiver (§28.10 / §28.11 / #2146); the receiver's owner is
//     the sole disposer.
//   §28.11 "ownership out" row: transfers elements, never the container.
//
// The gap is the same shape #2749 item 3 closed for the builtin-materializer
// predicate: `ast::NullCondMemberExpr` is a SIBLING of `ast::MemberAccessExpr`
// (not a subclass), so `dynamic_cast<MemberAccessExpr*>` returns null for every
// `?.` spelling and the predicate silently misses it. #2749 widened its
// predicate DIAGNOSTIC-ONLY; this predicate is NOT diagnostic-only —
// `isOwningCollectionDrainAliasCall` feeds codegen at
// src/codegen/LlvmBackend_Setup.cpp:13965/:14500 and ownership at
// src/ownership/Ownership.cpp:4316/:4753 — so widening changes disposal
// behaviour, which is the reason #2767 was filed separately and not folded
// into #2749.
//
// WHAT THIS FIX DOES NOT DO (load-bearing, mirrors #2749). Sema NEVER TYPES
// A BUILTIN INSTANCE CALL REACHED THROUGH `?.` (#2756 — measured by #2749:
// `sym=(nil) exprType=(nil)` on every builtin through `?.`, vs. `sym=0x…
// exprType=0x…` on the user-method twin). The widened predicate gates on the
// RECEIVER type (`ctx.exprType(*ma->target)`), which IS typed — the receiver
// is just a normal IdentifierExpr / field / etc. — so the predicate's gates
// pass. The const-rejection consumer reads `exprType(*ma->target)` and emits
// the §29.11 message; producerState's call-site arm returns Borrowed via the
// predicate's early-return; codegen's two registerUsingDisposal /
// registerIterableSourceDisposal sites set ownsSource=false. The same
// narrowness premise #2749 item 3 verified — `?.` types via a separate
// resolver (`ExprTC::resolveNullCondMember`) which attaches a MethodSymbol
// for user methods, so the symbol gate still excludes them — applies here
// unchanged.
//
// (1) `const q := a?.Drain();` — §21.1 REJECTS this. Today: ACCEPTED (bug —
//     predicate's callee cast returns null, no diagnostic fires). After fix:
//     REJECTED with the §29.11 message.
// (2) `using var d := a?.Drain();` over a named receiver — §28.10's
//     borrow-pin (§28.10 row "a method call without an `owned` return … is
//     a borrow"). Today: predicate doesn't fire, producerState defaults
//     Borrowed (the call is untyped per #2756 so there's no bz_array pointer
//     to mis-dispose — the value degrades to zero-init via #2434). After
//     fix: predicate fires, producerState explicitly returns Borrowed via
//     the predicate early-return. The runtime difference today is invisible
//     (sema still doesn't type the call); the SOUNDNESS value is that when
//     #2756 lands, the disposal path is correctly the receiver's owner, not
//     a second iterator-format disposal on a bz_array (the #2146 SEGV).
// (3) CONTROL — `?.Drain()` on a USER method over a stable named receiver.
//     Must keep its frame adoption (the user method IS a fresh iterator
//     factory): sema attaches a MethodSymbol on the `?.` callee, so the
//     symbol gate excludes it. Pinning the narrowness of the widening: a
//     wrong widening (e.g. dropping the symbol gate) would mis-classify it
//     and lose frame adoption — a user-frame leak.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Source-side scaffold — a tiny disposable class for the live-counter /
// runtime legs. The static field warning on `live` is a §22.12 rule 2 form
// (iii) admission under primitive-int storage in a single-threaded test
// driver — same scaffolding the #2146 file uses.
const char* kR =
    "module M; "
    "class R "
    "  public static live: int := 0; "
    "  public constructor() R.live := R.live + 1; end constructor; "
    "  destructor R.live := R.live - 1; end destructor; "
    "end R; ";

}  // namespace

// (1) §21.1 / §29.11 PIN — the literal #2767 repro: `const q := a?.Drain();`
// over a named using-var-owned `OwningArray<R>`. The spec rejects this:
// `Drain()` is a deferred-activation producer (single-consumption affine view,
// §29.11) and `const` may not bind an owned production. Today the predicate's
// callee cast (`dynamic_cast<MemberAccessExpr*>`) returns null for the
// `NullCondMemberExpr` callee, so the rejection site at Ownership.cpp:2580
// never fires — the program compiles clean. After widening to recognise
// `NullCondMemberExpr` (the same shape #2749 item 3 used for the
// builtin-materializer predicate), the rejection fires with the §29.11
// message.
BEZEL_TEST(nullcond_drain_alias_2767,
           const_over_nullcond_drain_rejected_spec_21_1_29_11) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(new R()); "
        "    const q := a?.Drain(); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "deferred-activation producer"));
}

// (1b) §21.1 / §29.11 PIN — the OwningDictionary variant. Same predicate,
// different stdlib §4.9 head, same shape the §21.1 row rejects. Today:
// same predicate miss, same accepted-but-buggy program.
BEZEL_TEST(nullcond_drain_alias_2767,
           const_over_nullcond_dictionary_drain_rejected_spec_21_1_29_11) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    using var d := new OwningDictionary<string, R>(); "
        "    d.Add(\"a\", new R()); "
        "    const q := d?.Drain(); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "deferred-activation producer"));
}

// (2) §28.10 SOUNDNESS PIN — `using var d := a?.Drain();`. The receiver is a
// named `using var` holding the `OwningArray`; the drain view aliases it
// (§28.10 borrow-pin: "no `Dispose()` is called, because the value is not
// owned by this scope"). Today the predicate's callee cast misses the `?.`
// form, but the call is untyped (#2756) and degrades to zero-init (#2434),
// so producerState's default-Borrowed is reached and no disposal is
// registered — there is no bz_array pointer to mis-dispose today. After
// widening the predicate, producerState returns Borrowed via the predicate
// early-return explicitly, and codegen's registerUsingDisposal /
// registerIterableSourceDisposal sites set ownsSource=false — so when #2756
// lands and `?.Drain()` becomes typed, the disposal path is the receiver's
// owner, not a second iterator-format disposal on a bz_array (the #2146
// SEGV). This test compiles cleanly and the program returns 0 in both
// states; the pin is the absence of a SEGV / double-dispose, plus the
// receiver's `live` counter returning to 0 (each R disposed once, by `a`).
#include "support/aot_support.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

BEZEL_TEST(nullcond_drain_alias_2767,
           using_var_over_nullcond_drain_no_double_dispose_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var a := new OwningArray<R>(); "
        "      a.Add(new R()); "
        "      a.Add(new R()); "
        "      using var drained := a?.Drain(); "
        "    end; "
        "    return R.live; "  // 0 — both elements disposed once, by `a`
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails if ASan fires (no SEGV today
                                    // because the call degrades to
                                    // zero-init per #2756 / #2434)
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (3) NARROWNESS CONTROL on the `?.` path — a USER method named `Drain`
// returning a fresh iterator frame (`IOwningEnumerable<int>`) over a named
// receiver. The user method IS a fresh-frame factory, NOT an alias; the
// symbol gate excludes it. Pinning the same narrowness #2749 item 3
// established for the materializer predicate: a wrong widening (e.g.
// dropping the symbol gate) would mis-classify this and lose frame
// adoption — a user-frame leak detectable by the second `R` instance
// surviving the for-loop's implicit source-disposal.
BEZEL_TEST(nullcond_drain_alias_2767,
           user_method_drain_via_nullcond_keeps_frame_adoption_spec_28_3) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class Factory "
        "  public _items: Array<R>; "
        "  public constructor(xs: Array<R>) _items := xs; end constructor; "
        "  public method Drain(): IOwningEnumerable<R> "
        "    yield return _items[0]; yield return _items[1]; "
        "  end Drain; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var arr := new Array<R>(); "
        "      arr.Add(new R()); arr.Add(new R()); "
        "      using var f := new Factory(arr); "
        "      var fbn: Factory? := f; "
        "      for var x in fbn?.Drain() do "
        "        x := new R(); "  // exercise the iteration
        "      end; "
        "    end; "
        "    return R.live; "  // 0 — both original R's disposed by `arr`'s
                             //      cascade; each loop-iter `x` re-binds to
                             //      a fresh R which is discarded at iter end
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
