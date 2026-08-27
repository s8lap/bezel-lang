// Codegen — issue #3404 (§30.9 Exception handling in async methods, §30.1
// `async` methods, spec v0.283): `collectSuspendLocals`' ONE-LINER CATCH-ARM
// branch carried a private second copy of the frame-slot-type derivation, whose
// default contradicted the rule #461 states 150 lines above it.
//
// #461 wrote the rule into the code, with its reason:
//
//     "When neither an annotation nor an initializer exprType is available,
//      default to a POINTER-WIDTH slot (not i32): ptr is large enough to hold
//      any scalar alloca (i32/i64/bool/ptr) without overflow, so a local with
//      an unresolved type can never clobber its neighbours."
//
// #3375 hoisted that derivation into a single `frameSlotTy(node, declType,
// init)` lambda so there would be exactly one copy of it — and deliberately
// left the `catch E => var a := …;` one-liner arm (#438) on its old private
// copy, whose default was `i32()`. This commit routes that branch through
// `frameSlotTy` too. The `const` sibling (`catch E => const a := …;`) already
// went through it; both siblings now share one derivation.
//
// ── WHAT AN UNDERSIZED SLOT ACTUALLY COSTS — MEASURED, NOT ASSUMED.
//
// The per-suspend spill stores `alloca->getAllocatedType()` bytes into the
// frame field (`emitAwaitSuspend` walks ALL of `smAllocaToField_` at EVERY
// suspend point — there is no liveness filter), so a ptr-typed arm local with
// an i32 field emits a mistyped 8-byte `store ptr` into 4 bytes. On a scratch
// compiler with this branch forced back to `i32()`, the first program below
// prints `n=0` instead of `n=7`: the arm local's spill overflowed its field and
// clobbered the adjacent one. A SILENTLY WRONG ANSWER, with no diagnostic and
// no sanitizer report — not a crash.
//
// It is also LAYOUT-DEPENDENT, which is the more useful half of the finding.
// The same program with the arm local's neighbour being an 8-aligned `ptr`
// rather than an `i32` ran perfectly clean on that same scratch compiler: the
// 4 bytes of alignment padding before the neighbour absorbed the entire
// overflow. An invariant whose violation is invisible in one field ordering and
// silently wrong in another is exactly the kind that must be stated once and
// consulted, never re-derived per branch.
//
// ── HONEST SCOPE: NO FAIL-WITHOUT ROW EXISTS, AND HERE IS WHY.
//
// This commit changes NO reachable behaviour. Both branches compute the same
// lowered type on every path where a type IS available, so a divergence needs
// an initializer whose `exprType` is null (or an annotation whose
// `resolvedType` is null), and every such shape tried was rejected by the front
// end or resolved after all:
//
//   `catch E => var d := [1, 2, 3];`     rejected: "collection literal has no
//                                        determinable target type — add a type
//                                        annotation (`var d: <CollectionType>
//                                        := [...]`) (§15.4)"
//   `catch E => var d := ["k": 1];`      rejected, same diagnostic
//   `catch E => var d := null;`          rejected
//   `catch E => var a := "hello";`       resolves — `ptr` field, both branches
//   `catch E => var a := await F();`     resolves through the AwaitExpr —
//                                        `ptr` field, both branches
//   `catch E => var a: T := v;` in an    resolves in the SPECIALIZED frame
//   unspecialized generic async method   (`ptr` for T=string); in the
//                                        unspecialized frame `lowerTypeRef` of
//                                        an unresolved type parameter is `i32`
//                                        on BOTH branches, so still no
//                                        divergence
//
// #808's `frameCapturedLocals_` ptr-widening — the second thing the private
// copy skipped — is unreachable here for a structural reason: a one-liner arm's
// body IS the declaration, so the local's scope ends with it and no lambda
// anywhere in the frame body can reference it.
//
// So the rows below are a RATCHET, not a regression test: they pass on the
// pre-fix compiler too, and they are what would go red if this branch (or the
// `const` one beside it) were ever narrowed again. They are demonstrably
// load-bearing rather than decorative — on the forced-`i32()` scratch compiler,
// measured:
//
//   string_local_in_one_liner_catch_arm_…      n=7  ->  n=0   (RED)
//   await_initialized_local_in_one_liner_…     n=7  ->  n=0   (RED)
//   const_sibling_one_liner_catch_arm_…        n=7  ->  n=7   (control: the
//                                                     `const` branch was
//                                                     already on frameSlotTy,
//                                                     so forcing the `var`
//                                                     branch narrow cannot
//                                                     touch it)
//
// per §30.9, §30.1 (issue #3404; #461's pointer-width rule, #3375's single
// derivation, #438's one-liner arm)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── A one-liner catch arm whose local is a `string` (a ptr alloca), with an
// `int` local declared AFTER the try so it takes the frame field immediately
// following the arm's. The arm runs, then a later `await` spills every
// registered suspend-local — including the arm's, whose scope has already
// ended, because the spill has no liveness filter. `n` is read after that
// await: it is the neighbour the overflow would land in. On a compiler whose
// arm slot is i32 this prints "r=3 n=0".
// per §30.9, §30.1 (issue #3404)
BEZEL_TEST(async_catch_arm_frame_slot_3404,
           string_local_in_one_liner_catch_arm_does_not_clobber_neighbour_spec_30_9_30_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var r := 3; "
        "    try "
        "      await Task.Delay(5); "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      Exception => var a := \"hello\"; "
        "    end; "
        "    var n := 7; "
        "    await Task.Delay(5); "
        "    Console.WriteLine(\"r=\" + r.ToString() + \" n=\" + n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "r=3 n=7\n");
}

// ── #438's own shape: the one-liner arm's local is initialized BY an `await`,
// so the arm itself contains a suspend point and the resume re-enters the catch
// region. The initializer's `exprType` resolves through the `AwaitExpr`
// (measured: a ptr field on both branches), so this is the shape that shows the
// divergence is not reachable here — and, with the same `n` neighbour, the
// shape that goes red if the branch is narrowed.
// per §30.9, §31.2 (issue #3404, #438)
BEZEL_TEST(async_catch_arm_frame_slot_3404,
           await_initialized_local_in_one_liner_catch_arm_spec_30_9_31_2) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static async method F(): Task<string> "
        "    await Task.Delay(2); "
        "    return \"abcdef\"; "
        "  end F; "
        "  public static async method Main(): Task<int> "
        "    var r := 3; "
        "    try "
        "      await Task.Delay(5); "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      Exception => var a := await C.F(); "
        "    end; "
        "    var n := 7; "
        "    await Task.Delay(5); "
        "    Console.WriteLine(\"r=\" + r.ToString() + \" n=\" + n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "r=3 n=7\n");
}

// ── CONTROL: the §21.1 `const` sibling of the arm above (#3375). It already
// derived its slot from `frameSlotTy`, so forcing the `var` branch narrow
// leaves this row green — which is what makes the two rows above evidence about
// the `var` branch specifically rather than about one-liner arms in general.
// The row is worth keeping on its own account: it pins that BOTH siblings now
// go through the one derivation.
// per §30.9, §21.1 (issue #3404, #3375)
BEZEL_TEST(async_catch_arm_frame_slot_3404,
           const_sibling_one_liner_catch_arm_shares_the_derivation_spec_30_9_21_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var r := 3; "
        "    try "
        "      await Task.Delay(5); "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      Exception => const a := \"hello\"; "
        "    end; "
        "    var n := 7; "
        "    await Task.Delay(5); "
        "    Console.WriteLine(\"r=\" + r.ToString() + \" n=\" + n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "r=3 n=7\n");
}
