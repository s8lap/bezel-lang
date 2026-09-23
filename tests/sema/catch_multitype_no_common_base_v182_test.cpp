// Spec-derived tests for §31.2.1's multi-type catch-arm BINDING rule.
//
// ── WHY THIS FILE READS AS A POLARITY FLIP ──────────────────────────────────
//
// This file was written at v0.182 to pin the rule its name records: "a
// multi-type catch-arm BINDING whose listed types share no common base class
// more specific than `Object` must be rejected". Proposal 0080 (graduated
// **v0.243**, issue #3046) DELETED that rule together with the permissive
// throw-operand ruling it existed to serve, so the file now pins what replaced
// it, on the same programs. The file and suite names are kept deliberately —
// renaming would break `git log`'s and the gate selector's line to the v0.182
// history that explains why these exact programs are the ones being asked.
//
// What changed, verbatim from §31.2.1 (v0.243):
//
//   * **Arm types are `Exception`-family.** "Every class type named in an arm
//     pattern — bare, binding, or listed in a multi-type alternative — must be
//     `Exception` or a class deriving from it ... an arm naming any other class
//     type is **provably dead** ... and is a **compile error** naming this
//     rule". So `Dog | Cat`, `Dog | IOException`, and even the single-type
//     `catch var e: Dog` are now rejected AT THE ARM TYPE — including in the
//     NON-binding form, which the v0.182 rule expressly left unrestricted.
//
//   * **The binding rule simplifies.** "When binding, `name` has the static
//     type of the **nearest common base** of the listed types ... Every listed
//     type is `Exception`-derived (the arm-type rule below), so the nearest
//     common base is always `Exception` or a class deriving from it; for
//     alternatives sharing nothing more specific, it is `Exception`." The
//     `Object`-base carve — and with it the rejection this file was named for
//     — is gone: under the arm-type rule the `Object` case is unreachable, so
//     the fallback is `Exception`, not an error. (v0.243 changelog: "the
//     `Object`-fallback rejection becomes unreachable rather than
//     repealed-with-changed-behavior" — for every program that remains
//     accepted the binding types identically.)
//
// ── WHAT STILL HOLDS FROM THE ORIGINAL FILE ─────────────────────────────────
//
// Issue #1624 (the NCB walk must TERMINATE on a cyclic base graph) and issue
// #1621 (the NCB must be CORRECT when a built-in exception type participates,
// rather than short-circuiting on `commonTypeOf`'s old declaration-less
// fallback) are untouched by v0.243 and keep their pins below, retargeted onto
// programs the arm-type rule admits where the original program no longer
// compiles. Their original rationale, preserved:
//
//   #1624: `ExprTC::catchArmNcb` computes the NCB via a base-set walk whose
//   visited-set guarded only the RESULT, not the WALK — a revisited node
//   re-pushed its bases, so a base-type cycle made the computation unbounded.
//
//   #1621: `commonTypeOf`'s `convertsInto` helper had a fallback ("Other kinds
//   ... conservatively treat same-kind as compatible") meant only for
//   different-KIND pairs, but it also fired whenever two Named TypeRefs reached
//   it with either side lacking a `declaration` (i.e. a built-in, which carries
//   no TypeSymbol). So `convertsInto(IOException, Dog)` spuriously returned
//   true and Step 1 returned the FIRST listed type as a bogus common type for
//   ANY built-in-involving list, short-circuiting Step 2 (the ancestor-name
//   walk that actually computes the NCB) entirely.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
// True iff the program produced the §31.2.1 v0.243 arm-type error for `type`.
bool hasDeadArmError(const std::string& src, const std::string& type) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`catch` arm type `" + type +
                           "` is not an exception type") != std::string::npos &&
            d.message.find("31.2.1") != std::string::npos)
            return true;
    }
    return false;
}
// True iff the program produced the v0.182 "no common base more specific than
// `Object`" rejection. §31.2.1 v0.243 DELETED that rule, so every assertion on
// this predicate below is a `false` — it must never fire again.
bool hasNoCommonBaseError(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("common base class more specific") != std::string::npos &&
            d.message.find("31.2.1") != std::string::npos)
            return true;
    }
    return false;
}
// True iff the program produced a §20 "no common type" error diagnostic (the
// `if`/`case`-expression common-typing rule, which — unlike §31.2.1's
// catch-arm NCB — does no ancestor/sibling search: #1621 / §20, verbatim,
// "there is no sibling-class least-upper-bound search (deliberately unlike
// the nearest-common-base rule of §31.2.1, which applies only to multi-type
// catch bindings)").
bool hasNoCommonType20Error(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no common type") != std::string::npos &&
            d.message.find("(§20)") != std::string::npos)
            return true;
    }
    return false;
}
}  // namespace

// ── §31.2.1 v0.243: the arm-type rule replaces the no-common-base rule ──────

// (c-flip) The v0.182 file's leading case, `catch var e: Dog | Cat`, is still
// rejected — but by a DIFFERENT rule, and the difference is the whole point of
// the flip: `Dog` and `Cat` are no longer "two classes without a shared base",
// they are two classes that cannot be thrown at all (§31.1), so the arm naming
// either of them is provably dead (§31.2.1). The old rejection must NOT fire.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           binding_two_unrelated_classes_rejected_by_arm_type_spec_31_2_1_v0_243) {
    const std::string src =
        "module M; class Dog: Exception end Dog; class Cat end Cat; "
        "class C public static method Main(): int "
        "  try throw new Dog(); "
        "  catch var e: Dog | Cat => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasDeadArmError(src, "Cat"), true);
    BEZEL_EXPECT_EQ(hasNoCommonBaseError(src), false);
}

// (c-flip) §31.2.1 v0.243: the NON-binding form of the same list — which the
// v0.182 rule expressly left "unrestricted" and which this file used to pin as
// ACCEPTED, running to exit 1 — is now rejected too. The arm-type rule is about
// the TYPES NAMED, not about the binding, so dropping the `var` is no longer a
// remedy. This is the single largest behavioural delta in the file.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           nonbinding_two_unrelated_classes_now_rejected_spec_31_2_1_v0_243) {
    const std::string src =
        "module M; class Dog: Exception end Dog; class Cat end Cat; "
        "class C public static method Main(): int "
        "  try throw new Dog(); "
        "  catch Dog | Cat => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasDeadArmError(src, "Cat"), true);
}

// (c-flip) §31.2.1 v0.243: a SINGLE-type binding arm naming a non-`Exception`
// class is rejected by the same rule. The v0.182 file pinned the opposite
// (`single_type_binding_unaffected_spec_31_2_1`: `catch var e: Dog` ran to
// exit 7) because the deleted rule examined only the multi-type case.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           single_type_binding_non_exception_class_rejected_spec_31_2_1_v0_243) {
    const std::string src =
        "module M; class Dog end Dog; "
        "class C public static method Main(): int "
        "  try throw new ArgumentException(\"x\"); "
        "  catch var e: Dog => return 7; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasDeadArmError(src, "Dog"), true);
}

// (c) §31.2.1 v0.243 — THE BINDING SIMPLIFICATION, and the load-bearing
// accept for it: "for alternatives sharing nothing more specific, it is
// `Exception`."
//
// An INTERFACE alternative is admissible in an arm (§31.2.1's carve: "an
// interface-typed arm remains admissible"), and an interface shares no CLASS
// base with a built-in exception — so `IRetryable | IOException` is exactly the
// shape whose NCB walk finds nothing. Under v0.182 that was the rejection this
// file was named for; under v0.243 the binding types as `Exception`, which is
// what makes `e.Message` resolve. Throwing an `IOException` selects the arm via
// the second alternative, so the returned length is observed end-to-end.
//
// VERIFIED to fail-without / pass-with the fix: pre-fix `catchArmNcb` returns
// null here and the caller reports "a multi-type `catch` arm that binds a name
// must list types sharing a common base class more specific than `Object`
// (§31.2.1)", so the program does not compile at all (r.ran == false, exit
// -999). Post-fix it compiles and returns 4.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           binding_no_shared_base_types_as_exception_spec_31_2_1_v0_243) {
    const std::string src =
        "module M; import System.IO; "
        "interface IRetryable end IRetryable; "
        "class C public static method Main(): int "
        "  try throw new IOException(\"boom\"); "
        "  catch "
        "    var e: IRetryable | IOException => do return e.Message.Length; end, "
        "    var f: Exception => do return 99; end "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasNoCommonBaseError(src), false);
    BEZEL_EXPECT_EQ(run(src), 4);  // "boom".Length = 4, via the Exception binding
}

// ── unchanged from v0.182: the NCB itself, on lists the arm-type rule admits ─

// §31.2.1: two user classes that share a user-declared `Exception`-derived base
// compile and bind at that base — the NCB is `AppErr`, so `e.Code()` resolves
// and runs (#593 behavior, regression guard).
BEZEL_TEST(catch_multitype_no_common_base_v182, binding_shared_user_base_resolves_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppErr: Exception "
        "  public constructor(msg: string, code: int) _code := code; end constructor; "
        "  private _code: int; "
        "  public virtual method Code(): int return _code; end Code; "
        "end AppErr; "
        "class AppErrA: AppErr "
        "  public constructor(msg: string, code: int) base(msg, code); end constructor; "
        "end AppErrA; "
        "class AppErrB: AppErr "
        "  public constructor(msg: string, code: int) base(msg, code); end constructor; "
        "end AppErrB; "
        "class C public static method Main(): int "
        "  try throw new AppErrA(\"test\", 42); "
        "  catch var e: AppErrA | AppErrB => return e.Code(); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        42);
}

// §31.2.1: two `Exception`-derived user classes bind at the built-in
// `Exception` NCB, so `e.Message` resolves without a "no member" error
// (the exact #593 repro). Must NOT report the deleted §31.2.1 rejection.
BEZEL_TEST(catch_multitype_no_common_base_v182, binding_exception_derived_still_ok_spec_31_2_1) {
    BEZEL_EXPECT_EQ(
        hasNoCommonBaseError(
            "module M; "
            "class ErrA: Exception "
            "  public constructor(msg: string) end constructor; "
            "end ErrA; "
            "class ErrB: Exception "
            "  public constructor(msg: string) end constructor; "
            "end ErrB; "
            "class C public static method Main(): int "
            "  try throw new ErrA(\"x\"); "
            "  catch var e: ErrA | ErrB => return e.Message.Length; "
            "  end; "
            "  return 0; "
            "end Main; end C;"),
        false);  // the v0.182 no-common-base rejection is deleted (v0.243)
}

// (a) §31.2.1 / #1624: a multi-type catch whose listed types share a base
// reached through a DEEP user hierarchy compiles in bounded time and resolves
// to the NCB. This exercises `catchArmNcb`'s base-set walk (Step 2): each
// listed type's ancestor chain is enumerated, and the walk must terminate
// (cycle-guarded + capped) while still finding `MidErr` as the NCB. Without
// the visited-set gating the walk, a deep / multi-path base graph blows up.
BEZEL_TEST(catch_multitype_no_common_base_v182, deep_shared_base_walk_terminates_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class MidErr: Exception "
        "  public constructor(msg: string, code: int) _code := code; end constructor; "
        "  private _code: int; "
        "  public virtual method Code(): int return _code; end Code; "
        "end MidErr; "
        "class LeafA: MidErr "
        "  public constructor(msg: string, code: int) base(msg, code); end constructor; "
        "end LeafA; "
        "class LeafB: MidErr "
        "  public constructor(msg: string, code: int) base(msg, code); end constructor; "
        "end LeafB; "
        "class C public static method Main(): int "
        "  try throw new LeafB(\"d\", 99); "
        "  catch var e: LeafA | LeafB => return e.Code(); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        99);  // NCB = MidErr; e.Code() returns the constructed _code
}

// §31.2.1: a single-type binding arm over an `Exception`-derived class binds at
// that type itself (the `types.size() == 1` shortcut — no NCB walk runs), and
// the arm-type rule admits it. The v0.243 sibling of this case, a single-type
// arm over a NON-`Exception` class, is rejected above.
BEZEL_TEST(catch_multitype_no_common_base_v182, single_type_binding_unaffected_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Dog: Exception end Dog; "
        "class C public static method Main(): int "
        "  try throw new Dog(); "
        "  catch var e: Dog => return 7; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        7);
}

// #1624 / §31.2.1 (NCB TERMINATION) — the load-bearing regression for the hang
// fix. The tests above all walk a NON-CYCLIC base graph (a 2-level diamond or a
// shallow shared base): no node is ever revisited, so dropping the cycle guard
// would NOT fail them. This case reproduces the actual compile-time hang that
// #1624 fixed: a CYCLIC base-type graph reaching `catchArmNcb`'s ancestor walk,
// where the pre-fix visited-set gated only the RESULT set, not the base WALK —
// a revisited node re-pushed its bases, so the walk looped forever.
//
// Spec grounding:
//  - §12.10: "An inheritance cycle is a compile error ... `class A: B`,
//    `class B: A` ... The compiler detects the cycle and reports an error
//    naming the participating types." So the cyclic pair below is REJECTED, but
//    — as the fix commit notes — it reaches the catch-arm binding via error
//    recovery: the §12.10 diagnostic is recorded, yet `typecheckExpressions`
//    still runs and reaches `catchArmNcb` on the (now cyclic) base graph.
//  - §31.2.1: a multi-type BINDING arm's static type is the nearest common base
//    of the listed types, computed by `catchArmNcb`. For `A | C` (A in the
//    A<->B cycle, C unrelated) `commonTypeOf(A, C)` is null — neither converts
//    into the other (isSubtype's cycle guard makes it return false for the
//    cyclic chain) — so the Step-2 ancestor walk on A runs and, pre-fix, never
//    terminates on the A->B->A->B cycle.
//
// v0.243 note: the arm types `A` and `C` are now ALSO rejected by §31.2.1's
// arm-type rule (neither derives from `Exception` — they cannot, being in a
// cycle with each other), and the throw operand by §31.1. Those rejections are
// diagnostics; they do not stop `catchArmNcb` from running, so the termination
// property this test exists for is still exercised on exactly the same graph.
// The assertion is deliberately NOT on which errors fire — it is that the
// compile FINISHES and that sema was reached.
//
// REPRODUCING THE BUG is only useful if a regression FAILS THE TEST rather than
// hanging CI. `runAot` (support/aot_support.hpp) already runs the front-end in
// a forked child under its own hard wall-clock COMPILE deadline
// (`aotCompileTimeoutSec`, 120s default, `BEZEL_AOT_COMPILE_TIMEOUT_SEC`
// overrides) and SIGKILLs + reaps on expiry. Pre-#1624 the walk loops forever
// in that forked compile child and the timeout fires, surfacing here as
// `r.ran == false` with a "did not finish" diagnostic.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           cyclic_base_graph_ncb_terminates_spec_31_2_1_1624) {
    // Cyclic base graph (§12.10) + an unrelated class C. `throw new C()`
    // instantiates the UNRELATED C (not a cyclic type), so the only unbounded
    // walk this input reaches — pre-fix — is `catchArmNcb`'s ancestor walk on
    // A's cyclic ancestor chain (the #1624 hang).
    const std::string src =
        "module M; "
        "class A: B end A; "          // cycle: A -> B -> A (§12.10)
        "class B: A end B; "
        "class C end C; "             // unrelated to A/B -> commonTypeOf(A,C) is
                                      // null, forcing catchArmNcb Step 2 onto
                                      // the cyclic ancestor walk
        "class Runner public static method Main(): int "
        "  try throw new C(); "
        "  catch var e: A | C => return 1; "  // §31.2.1 multi-type BINDING arm
        "  end; "
        "  return 0; "
        "end Main; end Runner;";

    auto r = runAot(src);

    // Assertion 1 (#1624): the §31.2.1 NCB walk TERMINATED on a cyclic base
    // graph — this is the load-bearing assertion. It fails iff the visited-set
    // cycle guard is dropped: the walk then loops forever in the forked compile
    // child, `runAot` hits its own compile deadline, and the rejection
    // diagnostic reads "did not finish" instead of carrying the sema errors.
    bool timedOut = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("did not finish within") != std::string::npos)
            timedOut = true;
    }
    BEZEL_EXPECT_FALSE(timedOut);
    if (timedOut) return;  // avoid a misleading second failure

    // Assertion 2: the §12.10 cycle was reached and reported, confirming sema
    // ran (vs. an early parse/scope abort that would also "not hang"). A
    // compile reject's stderr is captured as a single diagnostic whose message
    // is the whole driver stderr blob, so every error is findable as a
    // substring of it.
    bool foundCycle = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("circular inheritance detected") != std::string::npos)
            foundCycle = true;
    }
    BEZEL_EXPECT_TRUE(foundCycle);
}

// ── #1621: NCB reliability when a built-in exception type participates ────

// #1621 / §31.2.1 — THE load-bearing regression pin for that issue, FLIPPED by
// v0.243. Its program is the v0.182 changelog's own worked example,
// `Dog | IOException` (a mixed user-class / built-in list sharing no base more
// specific than `Object`), which v0.182 required to be rejected in BINDING form
// and admitted in non-binding form. Under v0.243 both forms are rejected, and
// by the ARM-TYPE rule: `Dog` is not `Exception`-derived, so no thrown value
// can ever match it.
//
// The #1621 defect this program was chosen to expose is still what makes the
// program interesting — it is the mixed declaration-less/declared Named pair
// that `commonTypeOf`'s old fallback answered wrongly — but the rule that
// catches it has moved one level earlier, to the arm type itself, which no
// `commonTypeOf` result can mask. The deleted rejection must not fire.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           mixed_user_and_builtin_binding_rejected_by_arm_type_spec_31_2_1_v0_243_1621) {
    const std::string src =
        "module M; import System.IO; "
        "class Dog end Dog; "
        "class C public static method Main(): int "
        "  try throw new ArgumentException(\"x\"); "
        "  catch var e: Dog | IOException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasDeadArmError(src, "Dog"), true);
    BEZEL_EXPECT_EQ(hasNoCommonBaseError(src), false);
}

// #1621 / §31.2.1 v0.243: the NON-binding form of the same mixed list, which
// v0.182 pinned as ACCEPTED and running to exit 1. Now rejected — the arm-type
// rule does not care whether the arm binds.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           mixed_user_and_builtin_nonbinding_now_rejected_spec_31_2_1_v0_243_1621) {
    const std::string src =
        "module M; import System.IO; "
        "class Dog end Dog; "
        "class C public static method Main(): int "
        "  try throw new ArgumentException(\"x\"); "
        "  catch Dog | IOException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasDeadArmError(src, "Dog"), true);
}

// #1621 / §31.2.1 v0.243: a three-way list mixing two SIBLING built-ins
// (`OverflowException`, `ArgumentException` — both `Exception`-derived, but
// neither a base of the other) with an unrelated user class (`Dog`) is rejected
// at `Dog`. Under v0.182 this was the case `commonTypeOf`'s buggy fold hid: it
// folded `OverflowException` the whole way through, so Step 2 never ran and no
// error was reported despite `Dog` sharing no base with the other two. The
// arm-type rule reaches `Dog` directly, so no fold can mask it.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           mixed_builtin_siblings_and_unrelated_user_class_rejected_spec_31_2_1_v0_243_1621) {
    const std::string src =
        "module M; "
        "class Dog end Dog; "
        "class C public static method Main(): int "
        "  try throw new ArgumentException(\"x\"); "
        "  catch var e: OverflowException | ArgumentException | Dog => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasDeadArmError(src, "Dog"), true);
}

// #1621 / §31.2.1: two SIBLING built-ins whose only common base is
// `Exception` (`OverflowException | ArgumentException`) must bind and
// resolve `.Message` — the exact scenario
// `codegen_593.two_builtin_types_message_resolves_spec_31_2_1`
// (tests/codegen/multitype_catch_ncb_593_test.cpp) exercises, but that test
// alone cannot distinguish a CORRECT `Exception` NCB from the pre-fix bug's
// WRONG first-listed-type answer (`.Message` resolves on either, since
// `isBuiltinExceptionType` fires for any built-in Named TypeRef — the
// masking this issue's own report flagged). This test does not by itself
// re-prove the NCB is exactly `Exception` (see the §20 sibling test below
// for that); it pins that the two-sibling-built-ins case still compiles and
// dispatches correctly after the fix (a regression guard for the built-in
// exception hierarchy walk added to `namedAncestorChain`, without which
// Step 2 cannot discover a PURELY built-in shared ancestor).
BEZEL_TEST(catch_multitype_no_common_base_v182,
           sibling_builtins_ncb_via_hierarchy_walk_spec_31_2_1_1621) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try throw new OverflowException(\"overflow\"); "
        "  catch var e: OverflowException | ArgumentException => "
        "    return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        8);  // "overflow".Length = 8
}

// #1621 / §31.2.1: the NCB of two sibling built-ins one level DEEPER than
// `Exception` — `OverflowException` and `DivideByZeroException` share the
// more specific `ArithmeticException` (both derive from it directly; per
// §31.2.1 the NCB is "the MOST SPECIFIC" shared base, not merely `Exception`
// — this pins that `namedAncestorChain`'s BFS-order (most-specific-first)
// walk stops at the first ancestor common to all listed types rather than
// over-generalizing to the v0.243 `Exception` fallback).
BEZEL_TEST(catch_multitype_no_common_base_v182,
           sibling_builtins_ncb_is_most_specific_shared_ancestor_spec_31_2_1_1621) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try throw new DivideByZeroException(\"div0\"); "
        "  catch var e: OverflowException | DivideByZeroException => "
        "    return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        4);  // "div0".Length = 4
}

// #1621 / §20 — the DEFINITIVE regression pin for the `convertsInto` root
// cause (independent of the "no member" masking that limits the sibling
// tests above): §20's `if`-expression common-typing rule does "no
// sibling-class least-upper-bound search (deliberately unlike the
// nearest-common-base rule of §31.2.1, which applies only to multi-type
// catch bindings)" (spec §20, v0.184). Two SIBLING built-in exception
// branches (`OverflowException` / `ArgumentException`, matching the
// catch-arm case above) must therefore be REJECTED here, even though the
// analogous catch-arm binding is ACCEPTED — the two rules are deliberately
// different, and this test would not be able to tell them apart if
// `convertsInto` were still spuriously returning `true` for any
// declaration-less Named pair.
//
// VERIFIED to fail-without / pass-with the #1621 fix: pre-fix,
// `convertsInto`'s buggy fallback made `commonTypeOf(OverflowException,
// ArgumentException)` return a (bogus) non-null common type, so this
// `if`-expression compiled with NO §20 diagnostic — the same underlying
// defect as the catch-arm bug, manifesting here as a SILENT ACCEPTANCE of a
// program §20 says must be rejected.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           if_expr_sibling_builtins_rejected_per_spec_20_1621) {
    const std::string src =
        "module M; class C "
        "  public static method Pick(cond: bool): Exception "
        "    return (if cond then new OverflowException(\"of\") else "
        "            new ArgumentException(\"ae\")); "
        "  end Pick; "
        "  public static method Main(): int return 0; end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(hasNoCommonType20Error(src), true);
}

// #1621 / §20 (positive control for the test above): a USER class that
// inherits a BUILT-IN exception base (`MyIoErr: IOException`) as one
// `if`-expression branch, against that SAME built-in base as the other
// branch, DOES have a common type (`IOException` — an ordinary upcast, §5.1
// / §12) and must compile and run. Pins that `convertsInto`'s ancestor-name
// resolution (used when one side has a declaration and the other does not)
// still accepts this legitimate case — the fix must not have overcorrected
// into rejecting every declaration-less pairing.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           if_expr_user_subtype_of_builtin_common_type_ok_spec_20_1621) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.IO; "
        "class MyIoErr: IOException "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end MyIoErr; "
        "class C "
        "  public static method Pick(cond: bool): IOException "
        "    return (if cond then new MyIoErr(\"custom\") else "
        "            new IOException(\"plain\")); "
        "  end Pick; "
        "  public static method Main(): int "
        "    var e := Pick(true); "
        "    return e.Message.Length; "
        "  end Main; "
        "end C;"),
        6);  // "custom".Length = 6
}

// ── #4644 / §31.2.1 + §5.1: the NCB binding must not be silently re-narrowed ─
//
// The three tests below pin the fix for corpus row `s31_2_1_0004`
// (tests/conformance/rules/cases/s31_2_1_0004.bzl, "a binding over unrelated
// types types at Exception, not either listed type").
//
// §31.2.1 (v0.307): "When binding, `name` has the static type of the **nearest
// common base** of the listed types … for alternatives sharing nothing more
// specific, it is `Exception`."
//
// `catchArmNcb` already computed that correctly — the tests earlier in this
// file prove it — but the RESULT was not load-bearing, because a built-in
// exception's TypeRef carries `declaration == nullptr` (built-ins have no
// TypeSymbol) and so fell through EVERY branch of `checkAssignable` into its
// terminal "Mixed / generic-parameter / null-literal cases: skip." So
//
//     catch var ex: SiblingA | SiblingB => var narrow: SiblingA := ex;
//
// was ACCEPTED, and with `throw new SiblingB("boom")` a `SiblingB` instance was
// reachable through a `SiblingA`-typed slot — an unchecked downcast that makes
// the NCB rule unobservable. The sibling shape `var n: int := ex` was worse
// still: it compiled and printed the exception object's ADDRESS.
//
// The pins assert the SPECIFIC diagnostic, not merely "was rejected": the arm
// types here are `Exception`-derived, so §31.2.1's provably-dead arm-type rule
// does NOT fire, and a bare "is rejected" oracle would pass on any future
// unrelated rejection of the same input (the #4716 vacuous-pass shape).

namespace {
// True iff `src` was rejected with the diagnostic carrying `needle`.
bool rejectedWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;   // it compiled and ran: not a rejection at all
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

// (b) #4644 / §31.2.1 + §5.1 — THE ROW. `SiblingA` and `SiblingB` share nothing
// more specific than `Exception`, so the binding's static type IS `Exception`;
// assigning it to a `SiblingA` slot is a narrowing reference conversion and
// §5.1 has none implicitly. Pre-fix this compiled and ran to exit 0.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           ncb_binding_not_implicitly_narrowed_to_listed_type_spec_31_2_1_4644) {
    const std::string src =
        "module M; "
        "class SiblingA: Exception "
        "  public constructor(message: string) base(message); end constructor; "
        "end SiblingA; "
        "class SiblingB: Exception "
        "  public constructor(message: string) base(message); end constructor; "
        "end SiblingB; "
        "class C public static method Main(): int "
        "  try throw new SiblingB(\"boom\"); "
        "  catch var ex: SiblingA | SiblingB => do "
        "    var narrow: SiblingA := ex; "
        "    return narrow.Message.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejectedWith(
        src, "the built-in exception type `Exception` is not a `SiblingA`"));
}

// (b) #4644 / §5.1 + §4.6 — the same fall-through, with SILENT WRONG CODE as
// the pre-fix behaviour rather than an unchecked downcast: `var n: int := ex`
// compiled and printed the exception object's address (probe-measured
// 864801568), a class reference reinterpreted as an `int`. §5.1 has no implicit
// conversion between a class reference and a value type.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           exception_binding_not_implicitly_converted_to_primitive_spec_5_1_4644) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  try throw new ArgumentException(\"boom\"); "
        "  catch var ex: Exception => do "
        "    var n: int := ex; "
        "    return n; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejectedWith(
        src, "is a class reference and the target is a value type"));
}

// (a) #4644 POSITIVE CONTROL — the UPCAST direction must stay accepted and must
// still RUN. §5.1's reference conversion from a derived class to its base is
// implicit, and the arm binding at `Exception` is exactly what makes
// `e.Message` resolve. Asserting the exit VALUE (not `compile_rc == 0`) is
// deliberate: a fix that makes something newly rejected can just as easily make
// the neighbouring accept lower to nothing.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           exception_upcast_from_user_derived_still_runs_spec_5_1_4644) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class SiblingA: Exception "
        "  public constructor(message: string) base(message); end constructor; "
        "end SiblingA; "
        "class SiblingB: Exception "
        "  public constructor(message: string) base(message); end constructor; "
        "end SiblingB; "
        "class C public static method Main(): int "
        "  try throw new SiblingB(\"boom\"); "
        "  catch var ex: SiblingA | SiblingB => do "
        "    var wide: Exception := ex; "
        "    return wide.Message.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        4);  // "boom".Length = 4, through the Exception-typed slot
}
