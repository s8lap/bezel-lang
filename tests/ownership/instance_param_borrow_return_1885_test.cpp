// Spec-derived tests for issue #1885 — §28.3 borrow-lifetime rule for an
// INSTANCE method's return (docs v0.196).
//
// §28.3: "A borrowed local may not be returned from a method at all unless the
// return is a borrow with a clearly-defined source that outlives the call — in
// this version: a receiver-sourced borrow from an instance method (a bare
// owned-field read, `this`, or a borrow reached through the receiver's fields
// and indexers), or the receiver-parameter borrow of an extension method ... A
// borrowed local or parameter with no source that outlives the call — e.g. a
// static method returning one of its parameters — may not be returned; this
// callee-side prohibition is what makes the caller-side sourceless-return
// owned-assumption sound."
//
// The compiler enforced this prohibition for STATIC methods
// (checkStaticBorrowReturn) but NOT for INSTANCE methods: an instance method
// could return a borrow reached through a plain parameter (`return a;` or
// `return h._inner;`), which the caller-side producers table mis-attributes to
// the RECEIVER (which outlives the call), while the borrow actually aliases the
// parameter's object — disposed by the caller while the return still aliases it
// (heap-use-after-free). checkInstanceBorrowReturn closes that hole.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// §28.3 instance-method parameter-sourced borrow-return diagnostic: cites
// §28.3, the "may not be returned from an instance method" text, and the
// offending parameter's name.
bool rejectsInstanceParamBorrowReturn(const OwnershipResult& r,
                                      std::string_view paramName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.3") == std::string::npos) continue;
        if (d.message.find("may not be returned from an instance method") ==
            std::string::npos)
            continue;
        if (d.message.find(std::string(paramName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — an instance method returning a plain-parameter-sourced borrow.
// =============================================================================

// The issue's field-sourced repro: `return h._inner;` reaches a field through
// the plain parameter `h`, not through the receiver. Before the fix this
// compiled clean and the returned borrow dangled once `h` was disposed.
BEZEL_TEST(instance_param_borrow_return_1885, field_through_parameter_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public V: int := 42; "
        "  public method Ping(): int => V; "
        "  destructor end destructor; end Inner; "
        "class Holder "
        "  public owned _inner: Inner; "
        "  public constructor() _inner := new Inner(); end constructor; "
        "end Holder; "
        "class Service "
        "  public method Extract(h: Holder): Inner "
        "    return h._inner; "
        "  end Extract; "
        "end Service;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsInstanceParamBorrowReturn(r, "h"));
}

// The issue's minimal repro (Variant B): an instance method returns a plain
// parameter directly. The exact `Pick` shape §28.3 names for static methods,
// here on an instance method — the caller attributes the return to the
// receiver, not to `a`.
BEZEL_TEST(instance_param_borrow_return_1885, bare_parameter_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public V: int := 9; "
        "  public method Ping(): int => V; "
        "  destructor end destructor; end Res; "
        "class Svc "
        "  public method Pick(a: Res, b: Res): Res "
        "    return a; "
        "  end Pick; "
        "end Svc;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsInstanceParamBorrowReturn(r, "a"));
}

// The `=>` expression-body form is checked by the same rule
// (runExprBodyOwningReturnCheck), so `=> a;` on an instance method is rejected
// identically to the block-body `return a;`.
BEZEL_TEST(instance_param_borrow_return_1885, expression_body_parameter_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class Svc "
        "  public method Pick(a: Res, b: Res): Res => a; "
        "end Svc;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsInstanceParamBorrowReturn(r, "a"));
}

// =============================================================================
// POSITIVE — no over-rejection: receiver-sourced returns stay admitted.
// =============================================================================

// A bare owned-field read `_inner` is the canonical receiver-sourced borrow
// (§28.6) — the source is the receiver's storage, which outlives the call.
BEZEL_TEST(instance_param_borrow_return_1885, bare_owned_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public constructor() end constructor; "
        "  destructor end destructor; end Inner; "
        "class Holder "
        "  public owned _inner: Inner; "
        "  public constructor() _inner := new Inner(); end constructor; "
        "  public method Get(): Inner => _inner; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The qualified `this._inner` form is equally receiver-sourced — its root is
// the `this` keyword, not a parameter.
BEZEL_TEST(instance_param_borrow_return_1885, this_qualified_owned_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public constructor() end constructor; "
        "  destructor end destructor; end Inner; "
        "class Holder "
        "  public owned _inner: Inner; "
        "  public constructor() _inner := new Inner(); end constructor; "
        "  public method Get(): Inner return this._inner; end Get; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A non-`owned` receiver field is a borrow reached through the receiver's
// fields (§28.6) — admitted, and its root is a field, not a parameter.
BEZEL_TEST(instance_param_borrow_return_1885, non_owned_receiver_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public constructor() end constructor; "
        "  destructor end destructor; end Inner; "
        "class Holder "
        "  public _ref: Inner; "
        "  public constructor(r: Inner) _ref := r; end constructor; "
        "  public method Get(): Inner => _ref; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Returning `this` itself is receiver-sourced — its root carries no parameter
// symbol, so the check admits it.
BEZEL_TEST(instance_param_borrow_return_1885, return_this_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Self(): Holder => this; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A borrow reached through the receiver's own field indexer (`_cache[key]`) is
// receiver-sourced (§28.6 "reached through the receiver's fields and
// indexers") — its root is the field `_cache`, not the parameter `key`.
BEZEL_TEST(instance_param_borrow_return_1885, receiver_field_indexer_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public constructor() end constructor; "
        "  destructor end destructor; end Inner; "
        "class Holder "
        "  public owned _cache: Array<Inner>; "
        "  public constructor(seed: Inner) "
        "    _cache := new Array<Inner>(); _cache.Add(seed); end constructor; "
        "  public method At(key: int): Inner => _cache[key]; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A value-typed return through a parameter is a COPY, not a borrow — outside
// the borrow-lifetime rule entirely (isBorrowableType is false), so it stays
// accepted.
BEZEL_TEST(instance_param_borrow_return_1885, value_typed_parameter_projection_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public V: int := 42; "
        "  public constructor() end constructor; "
        "  destructor end destructor; end Inner; "
        "class Svc "
        "  public method GetV(x: Inner): int return x.V; end GetV; "
        "end Svc;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 v0.194 S10 inheritance carve-out: a method IMPLEMENTING a generic
// declaration whose return type is a bare type parameter (`IChooser<T>.Pick(a:
// T, b: T): T`) inherits the bare-type-parameter return classification even
// though its own concrete signature names `C`. Callers dispatch through the
// generic signature and re-establish the borrow against their own arguments, so
// `return a;` is admitted here — NOT a §28.3 instance-return violation. Guards
// against over-rejecting the shape the argument-source-union rule admits.
BEZEL_TEST(instance_param_borrow_return_1885, inherited_bare_t_return_accepted) {
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

// The same inheritance carve-out for the `=>` expression-body form — the
// bare-T classification is inherited on both body kinds.
BEZEL_TEST(instance_param_borrow_return_1885, inherited_bare_t_expression_body_accepted) {
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
