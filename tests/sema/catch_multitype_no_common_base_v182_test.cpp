// Spec-derived tests for §31.2.1 (v0.182): a multi-type catch-arm BINDING
// whose listed types share no common base class more specific than `Object`
// must be rejected, and the nearest-common-base (NCB) computation that types a
// valid binding must TERMINATE (issue #1624) AND be CORRECT even when a
// built-in exception type participates (issue #1621, distinct from #1624 —
// see the dedicated section below).
//
// §31.2.1 (verbatim): "When binding, the listed types must share a common
// base class more specific than `Object` ... if the listed types' nearest
// common base is `Object`, the binding form is a compile error directing to
// separate arms — the non-binding form `Type1 | Type2` needs no common type
// and is unrestricted." And: "`name` has the static type of the nearest
// common base of the listed types (the most specific class that is a base of
// every listed type)."
//
// Issue #1624: `ExprTC::catchArmNcb` (src/sema/ExprTypecheck.cpp) computes the
// NCB via a base-set walk whose visited-set guarded only the RESULT, not the
// WALK — a revisited node re-pushed its bases, so a base-type cycle (or a deep
// diamond) made the computation unbounded. The walk is now cycle-guarded and
// capped; these tests pin both the §31.2.1 rejection and the termination of
// the shared-base resolution.
//
// Issue #1621 (fixed here, distinct from #1624 above): the NCB was not just
// occasionally slow (#1624) but OUTRIGHT WRONG whenever a built-in exception
// type participated in a multi-type catch-arm list. Root cause:
// `commonTypeOf`'s `convertsInto` helper (src/sema/ExprTypecheck.cpp) had a
// fallback —
//   // Other kinds (InlineArray/Function/Unit): conservatively treat
//   // same-kind as compatible to avoid false positives.
//   return true;
// — meant only for genuinely different-KIND pairs (InlineArray/Function/
// Unit), but it also fired whenever two Named TypeRefs reached it with
// either side lacking a `declaration` (i.e. a built-in, which carries no
// TypeSymbol). So `convertsInto(IOException, Dog)` spuriously returned
// `true`, and `catchArmNcb`'s Step 1 (the `commonTypeOf` fold) returned the
// FIRST listed type as a bogus "common type" for ANY built-in-involving
// list — short-circuiting before Step 2 (the ancestor-name walk that
// actually computes the NCB) ever ran. For `Dog | IOException` this bound
// `e` at `Dog` with NO §31.2.1 error, even though `Dog` and `IOException`
// share no base more specific than `Object` — precisely the v0.182
// changelog's own worked example of what §31.2.1 must reject.
//
// The fix (in `commonTypeOf` and a new `ExprTC::namedAncestorChain` /
// `hasNamedAncestor` pair, shared by `convertsInto` and `catchArmNcb`'s
// Step 2): a built-in-involving Named pair is now resolved by NAME through
// the same ancestor walk Step 2 uses — identity, a user-declared subtype of
// a built-in base, or a built-in ancestor/descendant pair — never a blanket
// "declaration-less means compatible" guess. The ancestor walk itself was
// extended to consult the built-in exception hierarchy
// (`common::builtinExceptionParentOf`, src/common/BuiltinRegistry.cpp,
// mirroring codegen's `Emitter::builtinExceptionHierarchy()`) once it
// reaches a built-in node with no further AST bases, so two SIBLING
// built-ins (`OverflowException | ArgumentException`, both `Exception`-
// derived but neither a base of the other) still resolve to their shared
// ancestor instead of silently mistyping the binding (the masked failure
// mode `codegen_593.two_builtin_types_message_resolves_spec_31_2_1`,
// tests/codegen/multitype_catch_ncb_593_test.cpp, could not detect: it only
// proves SOME built-in exception type was bound, via `.Message` resolving
// on any of them, not that the NCB itself is correct).

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
// True iff the program produced a §31.2.1 no-common-base error diagnostic.
bool hasNoCommonBaseError(const std::string& src) {
    auto r = runJit(src);
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
    auto r = runJit(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no common type") != std::string::npos &&
            d.message.find("(§20)") != std::string::npos)
            return true;
    }
    return false;
}
}  // namespace

// (c) §31.2.1: two unrelated user classes (no shared base beyond the implicit
// Object) in a BINDING multi-type arm is a compile error.
BEZEL_TEST(catch_multitype_no_common_base_v182, binding_two_unrelated_classes_rejected_spec_31_2_1) {
    BEZEL_EXPECT_EQ(hasNoCommonBaseError(
        "module M; class Dog end Dog; class Cat end Cat; "
        "class C public static method Main(): int "
        "  try throw new Dog(); "
        "  catch var e: Dog | Cat => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        true);
}

// §31.2.1: the same two types WITHOUT a binding (`Dog | Cat`, no `var`) are
// unrestricted — "the non-binding form ... needs no common type and is
// unrestricted." Must compile and run.
BEZEL_TEST(catch_multitype_no_common_base_v182, nonbinding_two_unrelated_classes_ok_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Dog end Dog; class Cat end Cat; "
        "class C public static method Main(): int "
        "  try throw new Dog(); "
        "  catch Dog | Cat => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// (b) §31.2.1: two user classes that DO share a user-declared base still
// compile and bind at that base — the NCB is `AppErr`, so `e.Code()`
// resolves and runs (#593 behavior, regression guard).
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

// (b) §31.2.1: two Exception-derived user classes bind at the built-in
// `Exception` NCB, so `e.Message` resolves without a "no member" error
// (the exact #593 repro). Must NOT report the new §31.2.1 rejection.
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
        false);  // must NOT report the §31.2.1 no-common-base error
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

// §31.2.1: a single-type binding arm is untouched by the no-common-base
// check (only the multi-type case is examined) — regression guard.
BEZEL_TEST(catch_multitype_no_common_base_v182, single_type_binding_unaffected_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Dog end Dog; "
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
// #1624 fixed: a CYCLIC base-type graph reaching `catchArmNcb`'s
// `collectAncestors` walk, where the pre-fix visited-set gated only the RESULT
// set, not the base WALK — a revisited node re-pushed its bases, so the walk
// looped forever.
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
//    cyclic chain) — so the Step-2 `collectAncestors(A)` walk runs and, pre-fix,
//    never terminates on the A->B->A->B cycle.
//
// REPRODUCING THE BUG is only useful if a regression FAILS THE TEST rather than
// hanging CI. The front-end (`runJit`) therefore runs in a forked child under a
// hard wall-clock deadline; the parent `waitpid`s and kills the child if it does
// not exit. Pre-fix, `collectAncestors` loops forever in the child and the
// parent kills it -> the test reports "did not terminate". Post-fix the child
// returns promptly, and the parent additionally asserts the §12.10 "circular
// inheritance" diagnostic was recorded — confirming the cycle was reached and
// reported (distinguishing a real termination from an early parse/scope abort),
// not bypassed. `runJit` does not run the ownership pass (off unless
// BEZEL_CHECK_OWNERSHIP=1) and skips codegen when sema has errors, so this path
// isolates the §31.2.1 sema NCB hang (#1624) from the separate cyclic-hierarchy
// codegen/ownership hangs tracked elsewhere.
//
// VERIFIED to fail-without / pass-with the fix: pre-fix bezelc on this input
// hangs in ExprTC::catchArmNcb's collectAncestors (push_back loop); post-fix it
// terminates reporting both the §12.10 cycle error and the §31.2.1
// no-common-base error.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           cyclic_base_graph_ncb_terminates_spec_31_2_1_1624) {
    // Cyclic base graph (§12.10) + an unrelated class C. `throw new C()`
    // instantiates the UNRELATED C (not a cyclic type), so the only unbounded
    // walk this input reaches — pre-fix — is `catchArmNcb`'s `collectAncestors`
    // on A's cyclic ancestor chain (the #1624 hang).
    const std::string src =
        "module M; "
        "class A: B end A; "          // cycle: A -> B -> A (§12.10)
        "class B: A end B; "
        "class C end C; "             // unrelated to A/B -> commonTypeOf(A,C) is
                                      // null, forcing catchArmNcb Step 2 onto
                                      // collectAncestors(A)'s cyclic walk
        "class Runner public static method Main(): int "
        "  try throw new C(); "
        "  catch var e: A | C => return 1; "  // §31.2.1 multi-type BINDING arm
        "  end; "
        "  return 0; "
        "end Main; end Runner;";

    namespace fs = std::filesystem;
    static int counter = 0;
    const std::string diagPath =
        (fs::temp_directory_path() /
         ("bezel_1624_" + std::to_string(counter++) + ".diag")).string();

    pid_t pid = ::fork();
    BEZEL_EXPECT_TRUE(pid >= 0);
    if (pid < 0) { std::remove(diagPath.c_str()); return; }

    if (pid == 0) {
        // Child: run the front-end on the cyclic graph. With the #1624 fix this
        // returns (sema records the §12.10 cycle + §31.2.1 no-common-base
        // errors, so runJit returns before codegen); without the fix
        // collectAncestors loops forever here and the parent kills this child.
        // Write the recorded errors to a file the parent inspects, then _Exit
        // (NOT exit): skip atexit/static destructors in the forked copy.
        auto r = runJit(src);
        std::ofstream out(diagPath);
        for (const auto& d : r.diagnostics.diagnostics()) {
            if (d.severity == bezel::Severity::Error)
                out << d.message << "\n";
        }
        out.flush();
        out.close();
        std::_Exit(0);
    }

    // Parent: bounded wait. A normal sema of this fragment is sub-second, so a
    // generous deadline still catches the pre-fix infinite walk quickly.
    constexpr int kDeadlineSec = 20;
    int status = 0;
    bool terminated = false;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(kDeadlineSec);
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { terminated = true; break; }
        if (w == -1) break;  // error other than EINTR; treat as not-terminated
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!terminated) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);  // reap the killed child
    }

    // Assertion 1 (#1624): the §31.2.1 NCB walk TERMINATED on a cyclic base
    // graph. This is the load-bearing assertion — it fails (killed child) iff
    // the visited-set cycle guard is dropped.
    BEZEL_EXPECT_TRUE(terminated);

    // Assertion 2: the §12.10 cycle was reached and reported, confirming sema
    // ran (vs. an early parse/scope abort that would also "terminate").
    std::string diags;
    {
        std::ifstream in(diagPath);
        diags.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }
    std::remove(diagPath.c_str());
    if (!terminated) return;  // avoid a misleading second failure
    BEZEL_EXPECT_TRUE(diags.find("circular inheritance detected") !=
                      std::string::npos);
}

// ── #1621: NCB reliability when a built-in exception type participates ────

// #1621 / §31.2.1 — THE load-bearing regression pin for this issue: the
// v0.182 changelog's own worked example, `Dog | IOException` (a mixed
// user-class / built-in list sharing no base more specific than `Object`),
// must be REJECTED in binding form.
//
// VERIFIED to fail-without / pass-with the fix: pre-fix, `convertsInto`'s
// buggy fallback made `commonTypeOf(Dog, IOException)` return `Dog` (the
// FIRST listed type, non-null) at Step 1, so `catchArmNcb` returned early —
// no §31.2.1 error, and `e` silently bound at `Dog` (confirmed by the
// companion assertion below: `e.Message` — a member IOException/Exception
// has but Dog does not — fails with "no member `Message` on type `Dog`",
// proving the pre-fix binding was typed `Dog`, not `Object` and not
// `Exception`). Post-fix, `convertsInto` resolves built-in-involving Named
// pairs by ancestor NAME instead of the blanket declaration-less fallback,
// so Step 1 correctly returns null (neither converts into the other) and
// Step 2's ancestor walk correctly finds no shared base short of `Object`.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           mixed_user_and_builtin_no_common_base_rejected_spec_31_2_1_1621) {
    const std::string src =
        "module M; import System.IO; "
        "class Dog end Dog; "
        "class C public static method Main(): int "
        "  try throw new Dog(); "
        "  catch var e: Dog | IOException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasNoCommonBaseError(src), true);
}

// #1621 / §31.2.1: the non-binding form of the same mixed list (`Dog |
// IOException`, no `var`) needs no common type and is unrestricted — must
// still compile and run. Regression guard: the fix must not over-reject the
// non-binding form the §31.2.1 rejection never applied to.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           mixed_user_and_builtin_nonbinding_ok_spec_31_2_1_1621) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.IO; "
        "class Dog end Dog; "
        "class C public static method Main(): int "
        "  try throw new Dog(); "
        "  catch Dog | IOException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// #1621 / §31.2.1: three-way list mixing two SIBLING built-ins
// (`OverflowException`, `ArgumentException` — both `Exception`-derived, but
// neither a base of the other) with an unrelated user class (`Dog`) must
// still be rejected. VERIFIED to fail-without the fix: pre-fix,
// `commonTypeOf`'s buggy fold made EVERY pairwise step in Step 1 return the
// running (first-listed) type non-null regardless of the actual pair — for
// this list it folds `OverflowException` the whole way through, so the
// walk never reaches Step 2 and no §31.2.1 error was ever reported despite
// `Dog` sharing no base with the other two.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           mixed_builtin_siblings_and_unrelated_user_class_rejected_spec_31_2_1_1621) {
    const std::string src =
        "module M; "
        "class Dog end Dog; "
        "class C public static method Main(): int "
        "  try throw new Dog(); "
        "  catch var e: OverflowException | ArgumentException | Dog => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(hasNoCommonBaseError(src), true);
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
// Step 2 cannot discover a PURELY built-in shared ancestor and this would
// wrongly regress into a §31.2.1 rejection).
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
// over-generalizing to the universal `Exception` fallback).
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
// VERIFIED to fail-without / pass-with the fix: pre-fix, `convertsInto`'s
// buggy fallback made `commonTypeOf(OverflowException, ArgumentException)`
// return a (bogus) non-null common type, so this `if`-expression compiled
// with NO §20 diagnostic — the same underlying defect as the catch-arm bug,
// manifesting here as a SILENT ACCEPTANCE of a program §20 says must be
// rejected. Post-fix, `convertsInto` correctly finds neither built-in
// converts into the other, so `commonTypeOf` returns null and §20's
// no-common-type error fires.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           if_expr_sibling_builtins_rejected_per_spec_20_1621) {
    const std::string src =
        "module M; class C "
        "  public static method Pick(cond: bool): Exception "
        "    return new OverflowException(\"of\") if cond else "
        "           new ArgumentException(\"ae\"); "
        "  end Pick; "
        "  public static method Main(): int return 0; end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(hasNoCommonType20Error(src), true);
}

// #1621 / §20 (positive control for the test above): a USER class that
// inherits a BUILT-IN exception base (`MyIoErr: IOException`) as one
// `if`-expression branch, against that SAME built-in base as the other
// branch, DOES have a common type (`IOException` — an ordinary upcast, §5.1
// / §12) and must compile and run. Pins that `convertsInto`'s new
// ancestor-name resolution (used when one side has a declaration and the
// other does not) still accepts this legitimate case — the fix must not
// have overcorrected into rejecting every declaration-less pairing.
BEZEL_TEST(catch_multitype_no_common_base_v182,
           if_expr_user_subtype_of_builtin_common_type_ok_spec_20_1621) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.IO; "
        "class MyIoErr: IOException "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end MyIoErr; "
        "class C "
        "  public static method Pick(cond: bool): IOException "
        "    return new MyIoErr(\"custom\") if cond else "
        "           new IOException(\"plain\"); "
        "  end Pick; "
        "  public static method Main(): int "
        "    var e := Pick(true); "
        "    return e.Message.Length; "
        "  end Main; "
        "end C;"),
        6);  // "custom".Length = 6
}
