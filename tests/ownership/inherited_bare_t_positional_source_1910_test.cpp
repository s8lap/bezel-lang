// Spec-derived tests for issue #1910 — §28.3 S10 *Inherited-bare-T* return
// carve-out is signature-blanket, not positional-source.
//
// §28.3 v0.196 S10 (Bare-type-parameter returns, callee side): a body
// implementing/overriding a generic declaration whose declared return type
// is a bare type parameter carries the bare-T return classification even
// though the impl's own signature names concrete types. Callers dispatch
// through the generic signature and re-establish the borrow against their
// own arguments, so the callee-side borrow-lifetime prohibition must admit
// returns through parameters that correspond POSITIONALLY to a SOURCE
// parameter in the inherited declaration (a plain parameter whose type
// mentions the returned type parameter).
//
// The #1885 fix's carve-out was a BLANKET per-method-signature skip:
// checkInstanceBorrowReturn returned early whenever
// `returnInheritsBareTypeParam_` was true, admitting ANY return expression
// (including a borrow through a NON-source parameter — a parameter at a
// position where the inherited declaration's parameter is NOT typed at the
// returned type parameter, e.g. a parameter typed `Other` instead of `T`).
// Such a return carries a borrow whose source is not re-established at
// every caller — the caller's argument at that slot is `Other`, not `T`,
// and may be disposed while the returned borrow still aliases it.
//
// Refinement: the carve-out must be keyed on the PARAMETER POSITION. A
// parameter at a source position in the inherited declaration (a position
// whose inherited type mentions the returned type parameter) admits the
// parameter-sourced borrow return; a parameter at any other position is
// still rejected, exactly as for a non-inherited method. The blanket skip
// is gone. // per §28.3 v0.196 S10, #1910

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) THE KEYSTONE REPRO for #1910. An impl `VChooser.Pick` overrides
// `IChooser<T>.Pick(a: T, b: T): T` — but the inherited declaration also
// has a third, NON-source parameter `c: Other`. The impl mirrors the
// inherited signature (same arity, same positional mapping), and the body
// returns through `c` (a parameter at a NON-source position in the
// inherited declaration — `Other` doesn't mention T, so it isn't a source).
//
// Before #1910 the blanket skip admitted this return (every impl of an
// inherited bare-T method was accepted regardless of the return expression),
// but the borrow's source is `c`, a parameter whose value the caller
// supplies at a slot that ISN'T typed at T — the caller can dispose `c`
// while the returned borrow still aliases it, a use-after-free.
//
// The fix refines the carve-out to admit only parameters at SOURCE
// positions; `c` is at position 2 where the inherited parameter is NOT
// `T`-typed, so the return is REJECTED. // per §28.3 v0.196 S10
// ---------------------------------------------------------------------------

BEZEL_TEST(inherited_bare_t_positional_source_1910,
           borrow_through_non_source_parameter_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C public constructor() end constructor; "
        "  destructor end destructor; end C; "
        "class Other public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Mark(): int return 1; end Mark; "
        "end Other; "
        "interface IChooser<T> "
        "  method Pick(a: T, b: T, c: Other): T; "
        "end IChooser; "
        "class VChooser: IChooser<C> "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Pick(a: C, b: C, c: Other): C return c; end Pick; "
        "end VChooser;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "may not be returned from an instance method"));
    BEZEL_EXPECT_TRUE(errorSays(r, "c"));
}

// ---------------------------------------------------------------------------
// (b) REGRESSION GUARD for the existing positive case — a parameter at a
// SOURCE position in the inherited declaration is still admitted. The
// shape `class VChooser: IChooser<T> ... method Pick(a: C, b: C): C return
// a;` from #1885 must continue to be accepted: both `a` and `b` are at
// source positions in `IChooser<T>.Pick(a: T, b: T): T` (their types
// mention T), so the refined carve-out admits the borrow through `a`.
// ---------------------------------------------------------------------------

BEZEL_TEST(inherited_bare_t_positional_source_1910,
           borrow_through_source_position_parameter_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C public constructor() end constructor; "
        "  destructor end destructor; end C; "
        "interface IChooser<T> "
        "  method Pick(a: T, b: T): T; "
        "end IChooser; "
        "class VChooser: IChooser<C> "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Pick(a: C, b: C): C return a; end Pick; "
        "end VChooser;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (c) REGRESSION GUARD for the `=>` expression-body form on the same source-
// position parameter — the bare-T classification is inherited on both body
// kinds, and `=> a;` over the source position is admitted.
// ---------------------------------------------------------------------------

BEZEL_TEST(inherited_bare_t_positional_source_1910,
           expression_body_source_position_parameter_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C public constructor() end constructor; "
        "  destructor end destructor; end C; "
        "interface IChooser<T> "
        "  method Pick(a: T, b: T): T; "
        "end IChooser; "
        "class VChooser: IChooser<C> "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Pick(a: C, b: C): C => a; "
        "end VChooser;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (d) OVER-REJECT GUARD — receiver-sourced return is admitted unchanged.
// `return _inner;` from an inherited-bare-T method whose `_inner` is a
// receiver-sourced owned field. The receiver-sourced check runs at a
// different layer of checkInstanceBorrowReturn (the root isn't a
// ParameterSymbol), and the refined carve-out is gated only on parameter-
// sourced returns — receiver/static-storage returns remain admitted.
// ---------------------------------------------------------------------------

BEZEL_TEST(inherited_bare_t_positional_source_1910,
           receiver_field_return_inherited_bare_t_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C public constructor() end constructor; "
        "  destructor end destructor; end C; "
        "interface IChooser<T> "
        "  method Pick(): T; "
        "end IChooser; "
        "class VChooser: IChooser<C> "
        "  public owned _inner: C; "
        "  public constructor() _inner := new C(); end constructor; "
        "  destructor end destructor; "
        "  public method Pick(): C return _inner; end Pick; "
        "end VChooser;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (e) OVER-REJECT GUARD — non-inherited concrete-typed method with a
// parameter-sourced borrow return remains rejected (the existing rule).
// The blanket skip was a carve-out for inherited-bare-T bodies ONLY — a
// plain concrete-typed method (no inherited bare-T classification) still
// rejects a borrow through any of its parameters, exactly as the
// checkInstanceBorrowReturn / #1885 rule has it. Mirrors the original
// `instance_param_borrow_return_1885.bare_parameter_rejected` test but
// keeps the regression guard against accidentally widening the reject
// away from inherited-bare-T bodies.
// ---------------------------------------------------------------------------

BEZEL_TEST(inherited_bare_t_positional_source_1910,
           non_inherited_concrete_typed_parameter_return_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class Svc "
        "  public method Pick(a: Res, b: Res): Res "
        "    return a; "
        "  end Pick; "
        "end Svc;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "may not be returned from an instance method"));
    BEZEL_EXPECT_TRUE(errorSays(r, "a"));
}