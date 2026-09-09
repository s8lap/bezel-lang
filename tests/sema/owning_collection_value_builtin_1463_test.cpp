// fix #1463 / §4.2 / §14.4: owning collections must reject ALL value-builtin
// element types, including the temporal builtins (DateTime/TimeSpan/DateOnly/
// TimeOnly), Guid, and CancellationToken.  Prior to this fix those six types
// silently compiled in `OwningArray<…>` because the null-declaration else-
// branch in TypeResolution_Names.cpp (~line 971) used a hardcoded name list
// that omitted them.  The fix introduces isValueBuiltinByName() (SemaContext.hpp)
// as a single authoritative predicate so name-list skew cannot recur.
//
// Spec: §4.2 ("owning collections hold owned class/interface elements"),
//       §14.4 ("Class-only restriction on owning generics").
// Related: #1327 (original owning-collection element check), #1464 (Guid fix
//           that exposed this — Guid became namable and the silent accept surfaced).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndResolveGated(src);
}

// True iff a diagnostic is the §4.2/§14.4 owning-collection element rejection.
bool hasOwningElementError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("requires a class or interface element type") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "interface IFoo method F(): int; end IFoo; "
        "class C public static method Main(): int ") + body +
        " return 0; end Main; end C;";
}

}  // namespace

// =============================================================================
// REJECTED — all six previously-slipping value-builtin element types in
// OwningArray (§4.2/§14.4/#1463).
// =============================================================================

// §4.2/§14.4: DateTime is a value-type builtin (§2.5) — rejected.
BEZEL_TEST(owning_collection_value_builtin_1463, datetime_in_owning_array_rejected) {
    auto r = parseSemaAll(wrap("using var a: OwningArray<DateTime> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §4.2/§14.4: TimeSpan is a value-type builtin (§2.5) — rejected.
BEZEL_TEST(owning_collection_value_builtin_1463, timespan_in_owning_array_rejected) {
    auto r = parseSemaAll(wrap("using var a: OwningArray<TimeSpan> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §4.2/§14.4: DateOnly is a value-type builtin (§2.5/#1357) — rejected.
BEZEL_TEST(owning_collection_value_builtin_1463, dateonly_in_owning_array_rejected) {
    auto r = parseSemaAll(wrap("using var a: OwningArray<DateOnly> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §4.2/§14.4: TimeOnly is a value-type builtin (§2.5/#1357) — rejected.
BEZEL_TEST(owning_collection_value_builtin_1463, timeonly_in_owning_array_rejected) {
    auto r = parseSemaAll(wrap("using var a: OwningArray<TimeOnly> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §4.2/§14.4/#1464: Guid is a value-type builtin (stdlib §2.6) — rejected.
BEZEL_TEST(owning_collection_value_builtin_1463, guid_in_owning_array_rejected) {
    auto r = parseSemaAll(wrap("using var a: OwningArray<Guid> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §4.2/§14.4: CancellationToken is a value-type builtin (§6.1) — rejected.
BEZEL_TEST(owning_collection_value_builtin_1463, cancellationtoken_in_owning_array_rejected) {
    auto r = parseSemaAll(wrap("using var a: OwningArray<CancellationToken> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// =============================================================================
// REJECTED — value-builtin elements across the rest of the owning family.
// =============================================================================

// §4.2/§14.4: OwningStack rejects a DateTime element.
BEZEL_TEST(owning_collection_value_builtin_1463, datetime_in_owning_stack_rejected) {
    auto r = parseSemaAll(wrap("using var s: OwningStack<DateTime> := new OwningStack<DateTime>();"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §4.2/§14.4/§28.11: OwningDictionary rejects a Guid VALUE (slot 1);
// the key slot (slot 0) is value-like and unrestricted.
BEZEL_TEST(owning_collection_value_builtin_1463, guid_as_owning_dict_value_rejected) {
    auto r = parseSemaAll(wrap("using var d: OwningDictionary<int, Guid> := new OwningDictionary<int, Guid>();"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// =============================================================================
// CONTROLS — confirm the fix did not over-reject.
// =============================================================================

// §14.4: int is a primitive value type — still correctly rejected (regression guard).
BEZEL_TEST(owning_collection_value_builtin_1463, int_in_owning_array_rejected_control) {
    auto r = parseSemaAll(wrap("using var a: OwningArray<int> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §14.4: decimal is a primitive value type — still correctly rejected.
BEZEL_TEST(owning_collection_value_builtin_1463, decimal_in_owning_array_rejected_control) {
    auto r = parseSemaAll(wrap("using var a: OwningArray<decimal> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §14.4: a class element is admitted (no false rejection).
BEZEL_TEST(owning_collection_value_builtin_1463, class_element_admitted_control) {
    auto r = parseSemaAll(wrap("using var a := new OwningArray<Foo>();"));
    BEZEL_EXPECT_FALSE(hasOwningElementError(r));
}

// §14.4: an interface element is admitted.
BEZEL_TEST(owning_collection_value_builtin_1463, interface_element_admitted_control) {
    auto r = parseSemaAll(wrap("using var a := new OwningArray<IFoo>();"));
    BEZEL_EXPECT_FALSE(hasOwningElementError(r));
}

// §14.4: a generic type parameter is admitted (not falsely recognised as a
// value-builtin by the null-declaration path).
BEZEL_TEST(owning_collection_value_builtin_1463, generic_type_param_admitted_control) {
    // Foo<T> uses OwningArray<T>; T is a generic param — should be admitted.
    //
    // fix #3985: `class Box<T: class>` inlined the constraint into the
    // generic-parameter list, but Appendix A puts constraints in a separate
    // `where Identifier ":" Constraint ...` clause after the parameter
    // list (`GenericParam ::= Variance? Identifier`, no constraint slot) —
    // this never parsed, so the row was reaching sema on parseSemaAll's
    // ungated, error-recovery-truncated AST. Correcting the generic-param
    // syntax alone then surfaces a second, independent defect the
    // truncated AST had also hidden: a fresh `new OwningArray<T>()` stored
    // into a plain (non-`owned`) field is a real §28.5 leak rejection, so
    // the field needs `owned` too (mirrors the pattern already used
    // throughout owning_element_generic_instantiation_2713_test.cpp, e.g.
    // `private owned f: OwningArray<T> := new OwningArray<T>();`).
    // VERIFIED against the real `bezelc -c`: the corrected source below
    // parses and typechecks with zero diagnostics.
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> where T: class "
        "  public owned items: OwningArray<T> := new OwningArray<T>(); "
        "end Box;");
    // fix #3997: an absence-only claim on ONE diagnostic also holds when
    // the fixture never parsed at all (gated by `parseSemaAll` ->
    // `parseAndResolveGated`). `!r.diagnostics.hasErrors()` is a real
    // discriminator: the gate makes "parsed+resolved clean" and "gated
    // off by a parse error" mutually exclusive.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasOwningElementError(r));
}
