// Codegen — issue #3614 (§21.1 runtime `const` locals, §28.9 runtime-managed
// string reference counting, spec v0.286): `Emitter::returnedStringLocalOwnsPlusOne`
// still re-derived the "is this local a +1 producer" answer via a LocalDecl-only
// `dynamic_cast`, even though the CANDIDATE SET it consults first
// (`stringLocalCandidates_`) has admitted a §21.1 runtime `const` since fix
// #3416. This is the residual, asymmetric half of #3416 that #3614 tracks: the
// collector was widened, this one reader was not.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp, ~line 20124, pre-fix):
//
//   if (!stringLocalCandidates_.count(declNode)) return false;   // TRUE for a const
//   auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);   // NULL for a const
//   if (ld && !ld->isUsing && ld->initializer && ...) { ... }
//   return false;                                                // always false
//
// `returnedStringLocalOwnsPlusOne` answers whether a returned string IDENTIFIER
// already owns the +1 it is handing the caller (a producer-initialized local
// whose slot holds the producer's transferred reference), so the RETURN-site
// codegen (LlvmBackend_Stmts.cpp) can TRANSFER it instead of minting a second
// one. For a `const s := <producer>; return s;`, `stringLocalCandidates_`
// already says "yes, a producer candidate" (post-#3416), but the LocalDecl-only
// cast made this function answer `false` anyway — so the return site mints an
// EXTRA `bz_string_retain` on top of the producer's existing +1. The escape walk
// (unrelated to this fix, and unchanged) already excludes a RETURNED local from
// `stringReleaseEligible_`, so `s`'s own scope end never balances the extra
// mint: one reference is leaked per call, unboundedly in a loop. This is a
// leak, never a use-after-free or double-free (the same "leak-direction, not a
// UAF" shape as #3427's three sibling sites).
//
// FIX. Read the producer/isUsing/type triple through `Emitter::localDeclInitView`
// — the single authority #3402 hoisted into LlvmBackend_impl.hpp, which maps
// LocalDecl -> {initializer, type, isUsing} and §21.1's LocalConstDecl ->
// {value, type, false} — instead of the LocalDecl-only dynamic_cast. No other
// consumer changes: this is the only reader of `loc->decl` in this function.
//
// §6b EXCLUSION DIFF. The pre-fix LocalDecl-only cast excluded EVERY §21.1
// `const` producer candidate unconditionally (an "always answer false for a
// const" default), forcing the return site's retain-mint path for all of them.
// `localDeclInitView` recognizes ONLY `ast::LocalDecl` and `ast::LocalConstDecl`
// (returning an all-null view, `{}`, for any other node kind) — identical to the
// pre-fix behavior for every non-LocalDecl node (the old `dynamic_cast` was also
// null there). So the widened reader admits EXACTLY the §21.1 `const` producer
// case beyond the unchanged `var` behavior; nothing else changes.
//
// ⚠ ORACLE NOTE: a SINGLE call's leaked +1 can still be reachable from the live
// call stack at LSan's exit-time scan (the same "still reachable" false negative
// #3416's own file documents), so every leak row below is a 200-iteration loop —
// the honest oracle, not a single execution.
//
// FAIL-WITHOUT / PASS-WITH, measured via `scripts/agent_check.sh --probe --asan`
// on one build of each compiler (the only difference being this fix's
// LlvmBackend_Setup.cpp hunk reverted), 200 iterations of
// `const s := "r" + n.ToString(); return s;`, the caller binding each call's
// result to a `var` that goes out of scope every iteration:
//
//   program                                          pre-fix           post-fix
//   const-returned producer, caller binds x200       7290 B / 400      clean
//   var-returned producer, caller binds x200 (ctrl)  clean             clean
//
// per §21.1, §28.9 (issue #3614)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── THE ISSUE'S OWN REPRO SHAPE, loop-amplified: a helper method returns a
// §21.1 runtime `const` string local bound to a `+` producer. Each of 200
// calls binds the result to a loop-scoped `var` that is released at its own
// scope end — so a correctly-transferred +1 is balanced exactly once per call,
// while a wrongly-MINTED extra +1 (the pre-fix bug) leaves one reference
// leaked per call. Sum of ("r"+i.ToString()).Length for i in [0,200) = 690
// (10 single-digit i's at length 2, 90 two-digit at length 3, 100 three-digit
// at length 4: 20+270+400). // per §21.1, §28.9 (issue #3614)
BEZEL_TEST(const_local_returned_string_3614,
           const_returned_string_producer_transferred_not_leaked_spec_21_1_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(n: int): string "
        "    const s := \"r\" + n.ToString(); "
        "    return s; "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var r := C.Make(i); "
        "      acc := acc + r.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "690\n");
}

// ── PARITY CONTROL: the `var` spelling of the identical program was clean
// pre-fix (returnedStringLocalOwnsPlusOne's LocalDecl-only cast already worked
// for it) and must stay clean post-fix — the widened reader must not change
// the `var` answer. // per §28.9 (issue #3614)
BEZEL_TEST(const_local_returned_string_3614,
           var_returned_string_producer_transferred_parity_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(n: int): string "
        "    var s := \"r\" + n.ToString(); "
        "    return s; "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var r := C.Make(i); "
        "      acc := acc + r.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "690\n");
}

// ── SAFETY GUARD (clean before AND after): a NON-producer `const` alias
// (`const s := q;`, a BORROW, not a producer) must not be mis-answered `true`
// by the widened reader — `isPlusOneStringProducer` must still decline a bare
// identifier initializer, exactly as it did for the `var` spelling, so the
// return site keeps minting the retain a borrow needs. Loop-amplified: a wrong
// `true` here would UNDER-retain and manifest as a use-after-free at the
// caller's read, not merely a leak — this is the double-checking direction
// #3416's own file calls out as the one that actually risks a UAF.
// // per §21.1, §28.9 (issue #3614)
BEZEL_TEST(const_local_returned_string_3614,
           const_returned_borrow_alias_still_retained_no_uaf_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(n: int): string "
        "    var q := \"base\" + n.ToString(); "
        "    const s := q; "
        "    return s; "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var r := C.Make(i); "
        "      acc := acc + r.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        // "base"+i.ToString(): length 5 for i in [0,9] (10), 6 for i in [10,99]
        // (90), 7 for i in [100,199] (100) = 50 + 540 + 700 = 1290.
        "1290\n");
}
