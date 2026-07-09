// Spec-derived tests for §31.2.1 (v0.182): a multi-type catch-arm BINDING
// whose listed types share no common base class more specific than `Object`
// must be rejected, and the nearest-common-base (NCB) computation that types a
// valid binding must TERMINATE (issue #1624).
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
// NOTE ON SCOPE: this fix covers the case where the NCB walk runs on ordinary
// USER classes (the §31.2.1 "no common base" rejection fires correctly). A
// separate, pre-existing sema defect (#1621) means the NCB is unreliable when
// a BUILT-IN exception type participates in the list — `commonTypeOf`'s
// `convertsInto` fallback spuriously treats built-in Named types as mutually
// convertible, so `catchArmNcb`'s Step 1 returns a wrong non-null answer
// before the corrected Step 2 walk runs. That case is tracked by #1621 and is
// NOT covered here.

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
