// Spec-derived tests for fix #4212 — Family I1 anchor (§14.1, §27.8, v0.298):
// hoist one `sema::substituteGenericArgs` over the previously separate
// `Resolver::applyGenericSubst`, `Resolver::substituteAliasGenericArgs`,
// and `ExprTC::substituteGenericFieldType` walkers. All three callers now
// delegate to the shared authority; the rows below exercise every TypeRef
// kind the walker handles (Named with genericArgs, InlineArray element,
// Function params/return, nullable preservation, alias), proving each
// caller path produces the SAME substitution outcome.
//
// §14.1 "Generic types" (v0.298): every bare type-parameter reference
// inside a generic body must substitute to the concrete instantiation
// argument, recursively into Named-with-genericArgs, InlineArray element,
// Function param/return positions.
//
// §27.8 "Type aliases": "Each instantiation of the alias is interchangeable
// with the corresponding instantiation of the aliased type." A generic
// alias referenced with `<int>` is identical to the aliased type referenced
// with `<int>`. The walker threads substitution through alias reference
// sites so this interchangeability is byte-exact.

#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

// ---------------------------------------------------------------------------
// §14.1 — Named with genericArgs: a buried parameter inside a concrete
// generic must substitute to the concrete argument. Exercises the
// `Resolver::applyGenericSubst` caller (the override-signature path) — the
// pre-fix walker only substituted a TOP-LEVEL bare parameter, so a method's
// return type `Array<T>` over `Container<int>` was left as `Array<T>`.
// ---------------------------------------------------------------------------

// A generic method declared on a concrete-instantiation base class whose
// return type is `Array<T>` (T from the base): after fix #4212 the call to
// the inherited method returns an `Array<int>`, which the caller can
// iterate. Pre-fix: the substitution walker only handled a top-level bare
// parameter, so `Array<T>` was left intact and the program IR-verified or
// failed at the .Length access (the substitution miss surfaces differently
// per call shape). Post-fix: returns an empty Array<int> with Length 0.
BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           named_with_generic_args_buried_in_base_return_spec_14_1) {
    const std::string prog =
        "module M; "
        "class Base4212<T> "
        "  public method Get(): Array<T>? "
        "    return null; "
        "  end Get; "
        "end Base4212; "
        "class Derived4212: Base4212<int> "
        "end Derived4212; "
        "public static class C "
        "  public static method Main(): int "
        "    var d := new Derived4212(); "
        "    var a := d.Get(); "
        "    if a = null then return 0; end; "
        "    return a.Length; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Same shape via a TYPE-PARAMETER-typed local whose declared type is
// `Array<T>` — the substitution must recurse so `a.Length` resolves at
// `Array<int>` (not at bare-T's "no members").
BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           named_with_generic_args_local_var_array_t_spec_14_1) {
    const std::string prog =
        "module M; "
        "class Box4212<T> "
        "  public property V: Array<T> := null; "
        "end Box4212; "
        "public static class C "
        "  public static method Main(): int "
        "    var b := new Box4212<int>(); "
        "    return b.V.Length; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// §15.1 — InlineArray element: the walker must recurse into an inline
// array's element type so a `[N]T` field at `Container<int>` substitutes to
// `[N]int`. Exercises the `Resolver::applyGenericSubst` caller.
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           inline_array_element_substitutes_through_generic_spec_14_1_15_1) {
    const std::string prog =
        "module M; "
        "class InlineBox4212<T> "
        "  public f: [3]T; "
        "end InlineBox4212; "
        "public static class C "
        "  public static method Main(): int "
        "    var b := new InlineBox4212<int>(); "
        "    return b.f.Length; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// §14.1 — Function params/return: a generic method whose parameter list
// and return type BOTH carry type parameters must substitute at every
// position. Exercises the `Resolver::applyGenericSubst` caller.
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           function_params_and_return_substitute_at_every_position_spec_14_1) {
    const std::string prog =
        "module M; "
        "class FBox4212<T> "
        "  public method Id(x: T): T "
        "    return x; "
        "  end Id; "
        "end FBox4212; "
        "public static class C "
        "  public static method Main(): int "
        "    var b := new FBox4212<int>(); "
        "    return b.Id(55); "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// ---------------------------------------------------------------------------
// §14.1 — Nullable preservation: a `T?` field must substitute to `int?`
// (the existing nullable-marker copy survives substitution). Exercises
// ALL THREE callers' nullable arms through different surfaces.
// ---------------------------------------------------------------------------

// applyGenericSubst caller: base-class field declared as `T?`.
BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           nullable_preserved_through_override_param_spec_14_1) {
    const std::string prog =
        "module M; "
        "class NBox4212<T> "
        "  public method TryGet(): T? "
        "    return null; "
        "  end TryGet; "
        "end NBox4212; "
        "public static class C "
        "  public static method Main(): int "
        "    var n := new NBox4212<int>(); "
        "    var v: int? := n.TryGet(); "
        "    if v = null then return 7; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// substituteGenericFieldType caller: a generic CHOICE whose payload is
// `T?` must bind the destructured slot at `int?` (not bare-T), so
// `v = null` compiles and yields true. Pre-fix would bind at bare T,
// which is incompatible with the §5.1 nullable-aware conversion.
BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           nullable_preserved_through_choice_payload_spec_14_1_35_5) {
    const std::string prog =
        "module M; "
        "choice Maybe4212<T> Just(value: T?), Nothing; end Maybe4212; "
        "public static class C "
        "  public static method Of(m: Maybe4212<int>): int "
        "    case m of "
        "      Just(v) => do if v = null then return 11; end; return 0; end, "
        "      Nothing => do return 12; end "
        "    end; "
        "    return 0; "
        "end Of; "
        "public static class P "
        "  public static method Main(): int "
        "    return C.Of(Maybe4212<int>.Just(null)); "
        "  end Main; "
        "end P; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// ---------------------------------------------------------------------------
// §27.8 — Alias substitution: a generic alias referenced with `<int>` must
// be byte-identical to the aliased type referenced with `<int>`. Exercises
// the `Resolver::substituteAliasGenericArgs` caller.
// ---------------------------------------------------------------------------

// Identity alias (`type IdP11d<T> := T;`) — the post-#4188 baseline: this
// passes today on HEAD, the fix preserves it, and the regression rows
// below prove the unified walker does not regress this shape.
BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           identity_alias_instantiated_concretely_unchanged_spec_27_8) {
    const std::string prog =
        "module M; "
        "type IdP4212<T> := T; "
        "public static class C "
        "  public static method Main(): int "
        "    var x: IdP4212<int> := 9; "
        "    return x; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// Nested generic alias (`type AliasPair<T> := Pair4212<T, int>;`) —
// §27.8's own example shape, here substituted at every depth. Pre-fix
// the alias walker substituted but missed the buried `T` on the second
// slot when the alias body itself was a Named-with-genericArgs whose
// genericArgs were non-trivial.
BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           generic_alias_with_buried_param_substitutes_spec_27_8) {
    const std::string prog =
        "module M; "
        "record Pair4212<A, B>(First: A, Second: B); "
        "type AliasPair4212<T> := Pair4212<T, int>; "
        "public static class C "
        "  public static method Main(): int "
        "    var p: AliasPair4212<string> := new Pair4212<string, int>(\"x\", 4); "
        "    return p.Second; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// ---------------------------------------------------------------------------
// §15.1 — Nested-inline-array alias: `type Nested<T> := [1]T;` referenced
// with an inline-array argument `[1]string` triggers the §15.1 re-validation
// gate (alias-only behaviour, kept via `validateNestedInlineArrays`). This
// row is the FAMILY's discriminator: it fails BECAUSE the alias
// substitution is now central — the unified walker must still re-validate
// after substituting. Pre-fix: report rejected. Post-fix: same.
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           nested_inline_array_alias_still_rejects_spec_15_1_27_8) {
    auto r = runAot(
        "module M; "
        "type Nested4212<T> := [1]T; "
        "public static class C "
        "  public static method Main(): int "
        "    var n: Nested4212<[1]string>; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // The pre-fix `substituteAliasGenericArgs` reported this from the
    // alias reference site; the unified walker must do the same. A
    // clean ACCEPT here would mean the alias-specific arm was lost.
    // Per #3892 burn-down: a bare `!r.ran` cannot distinguish a genuine
    // compile-time rejection from a runtime crash/signal/timeout/ASan
    // report, so also pin a real source location in the diagnostic
    // (see tests/support/aot_result_asserts.hpp).
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// §14.1 / §27.8 — CONSISTENCY ACROSS CALLERS: a single program that
// exercises all three eager walkers (`Resolver::applyGenericSubst` on the
// override return, `Resolver::substituteAliasGenericArgs` on the alias
// reference, `ExprTC::substituteGenericFieldType` on the generic field)
// against the SAME concrete instantiation `<int>` and proves the three
// substitutions produce the SAME observable value. The unification the
// hoist is supposed to guarantee — if any caller path drifts from the
// others, this row fails to compile or returns 0 instead of 21.
//
// This is the structural analogue of the §3434 precedent: that hoist
// consolidated two callers of "does this TypeRef produce a value?" into
// `sema::ascribedReturnTypeProducesValue`; this row verifies three
// callers of "what does this TypeRef substitute to?" all hit the same
// `sema::substituteGenericArgs`.
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           three_callers_substitute_identically_spec_14_1_27_8) {
    auto r = runAot(
        "module M; "
        "class Base3Callers4212<T> "
        "  public method GetT(): T? "
        "    return null; "
        "  end GetT; "
        "end Base3Callers4212; "
        "class Derived3Callers4212: Base3Callers4212<int> "
        "end Derived3Callers4212; "
        "type IdP3Callers4212<T> := T; "
        "class Box3Callers4212<T> "
        "  public V: T; "
        "end Box3Callers4212; "
        "public static class C "
        "  public static method Main(): int "
        // applyGenericSubst path: override resolution returns T, T → int
        "    var d := new Derived3Callers4212(); "
        "    var a: int? := d.GetT(); "
        // substituteGenericFieldType path: Box<int>.V is int
        "    var b := new Box3Callers4212<int>(); "
        "    var bb: int := b.V; "
        // substituteAliasGenericArgs path: IdP3Callers<int> is interchangeable with int
        "    var c: IdP3Callers4212<int> := 21; "
        "    var cc: int := c; "
        "    if a = null and bb = 0 and cc = 21 then return 21; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 21);
}

// ---------------------------------------------------------------------------
// §14.1 — MULTI-PARAMETER GENERIC: a record with TWO type parameters
// (`MPair<A, B>`) must substitute at EVERY position independently — A→int
// on `First`, B→string on `Second`. A walker that substituted only the
// FIRST parameter, or only the bare-name leaf, would fail to compile this
// row.
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           multi_parameter_generic_substitutes_at_every_position_spec_14_1) {
    auto r = runAot(
        "module M; "
        "record MPair4212<A, B>(First: A, Second: B); "
        "public static class C "
        "  public static method Main(): int "
        "    var p := new MPair4212<int, string>(3, \"xy\"); "
        // substituteGenericFieldType: First: A → int, Second: B → string
        "    if p.First = 3 and p.Second.Length = 2 then "
        "      return 23; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 23);
}

// ---------------------------------------------------------------------------
// §14.1 — DEEPLY NESTED GENERIC (depth-3 recursion): `Outer<T>` holds
// `Middle<T>` holds `Inner<T>` holds `V: T`. After `Outer<int>` the
// walker must recurse THREE levels so `o.Middle.Inner.V` reads at `int`,
// not at bare-T's "no members". A walker that did only one level of
// recursion would leave `Middle.Inner` typed `Middle<T>` (with T still
// unbound) and the inner-field access would fail type resolution.
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           deeply_nested_generic_substitutes_three_levels_spec_14_1) {
    auto r = runAot(
        "module M; "
        "class NestInner4212<T> "
        "  public V: T; "
        "end NestInner4212; "
        "class NestMiddle4212<T> "
        "  public owned Inner: NestInner4212<T> := new NestInner4212<T>(); "
        "end NestMiddle4212; "
        "class NestOuter4212<T> "
        "  public owned Middle: NestMiddle4212<T> := new NestMiddle4212<T>(); "
        "end NestOuter4212; "
        "public static class C "
        "  public static method Main(): int "
        "    var o := new NestOuter4212<int>(); "
        // After 3 levels of substitution, o.Middle.Inner.V is int (default 0)
        "    if o.Middle.Inner.V = 0 then return 27; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 27);
}

// ---------------------------------------------------------------------------
// §14.1 — REPEATED INSTANTIATION (cache miss / generic-spec cache):
// a single program that instantiates the SAME generic `<int>` SIX times
// at distinct call sites — every call must produce the same substituted
// type. If the unified walker caches by structural shape rather than by
// instantiation identity, every one of these reads as int. If a future
// refactor accidentally keyed the cache by pointer (so each access
// re-walked the substitution) the values still match — but the test
// pins the CONSISTENCY that is the load-bearing guarantee of "one
// authority, three callers".
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_args_unified_walker_4212,
           repeated_instantiation_produces_consistent_substitution_spec_14_1) {
    auto r = runAot(
        "module M; "
        "class RepBox4212<T> "
        "  public V: T; "
        "end RepBox4212; "
        "public static class C "
        "  public static method Main(): int "
        "    var b1 := new RepBox4212<int>(); "
        "    var b2 := new RepBox4212<int>(); "
        "    var b3 := new RepBox4212<int>(); "
        "    var b4 := new RepBox4212<int>(); "
        "    var b5 := new RepBox4212<int>(); "
        "    var b6 := new RepBox4212<int>(); "
        // All six V fields substituted to int — sum 0 (defaults).
        "    var sum := b1.V + b2.V + b3.V + b4.V + b5.V + b6.V; "
        "    if sum = 0 then return 31; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 31);
}