// fix #761 / §14.4: the standard-library owning collection types are
// compiler-enforced to reject runtime-managed reference element types
// (`string`, function values).
//
// §14.4 ("Class-only restriction on owning generics"): "the standard-library
// owning types are compiler-enforced to reject the runtime-managed reference
// types (`string`, function values). The reason: those types are runtime-
// managed (§28.9) and have no disposal obligation; owning them is degenerate.
// The error message at instantiation names the rejected type …"
// §3.6: such a property-dependent requirement "is rejected *at the
// instantiation site*" — so the check fires for every spelling of the type, not
// only `new`. §14.4 also states interfaces and nullable class types remain
// admitted, and for `OwnedDictionary<K, V>` the owned slot is the VALUE V (keys
// are value-like, §28.11).
//
// The under-enforcement this file pins down (and #761 fixes): the owning
// collection family (`OwnedArray`, `OwnedSet`, `OwnedStack`, `OwnedQueue`,
// `OwnedLinkedList`, `OwnedDictionary`) accepted a `string` / function element
// with no diagnostic, in every spelling.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some emitted diagnostic is the §14.4 owning-collection element
// rejection (so the "rejected" tests assert THIS check fired, not an unrelated
// error, and the "admitted" tests assert it did not).
bool hasOwningElementError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("owning collection types reject runtime-managed") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string wrap(const std::string& body) {
    return std::string(
        "module M; class Foo public n: int := 0; end Foo; "
        "interface IFoo method F(): int; end IFoo; "
        "class C public static method Main(): int ") + body +
        " return 0; end Main; end C;";
}

}  // namespace

// =============================================================================
// REJECTED — `string` / function element in an owning collection (§14.4).
// =============================================================================

// §3.6 / §14.4: the repro — a TYPED LOCAL (no `new`) must be rejected at the
// instantiation site.
BEZEL_TEST(owning_collection_element_761, typed_var_owned_array_string_rejected) {
    auto r = parseSemaAll(wrap("using var arr: OwnedArray<string> := [];"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §14.4: the `new` spelling is equally rejected.
BEZEL_TEST(owning_collection_element_761, new_owned_array_string_rejected) {
    auto r = parseSemaAll(wrap("using var arr := new OwnedArray<string>();"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §14.4 / §28.11: `OwnedDictionary<K, V>` owns its VALUES — a `string` VALUE is
// rejected (the key slot is value-like and unrestricted).
BEZEL_TEST(owning_collection_element_761, owned_dictionary_string_value_rejected) {
    auto r = parseSemaAll(wrap("using var d := new OwnedDictionary<int, string>();"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §14.4: the restriction covers the whole owning family — Stack here.
BEZEL_TEST(owning_collection_element_761, owned_stack_string_rejected) {
    auto r = parseSemaAll(wrap("using var s := new OwnedStack<string>();"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// §14.4: a function value is the other runtime-managed reference type rejected.
BEZEL_TEST(owning_collection_element_761, owned_array_function_rejected) {
    auto r = parseSemaAll(wrap("using var a := new OwnedArray<() -> int>();"));
    BEZEL_EXPECT_TRUE(hasOwningElementError(r));
}

// =============================================================================
// ADMITTED — class / interface / nullable-class elements, value-like keys, and
// the borrowing family (§14.4).
// =============================================================================

// §14.4: a proper class element is the canonical admitted case.
BEZEL_TEST(owning_collection_element_761, owned_array_class_admitted) {
    auto r = parseSemaAll(wrap("using var a := new OwnedArray<Foo>();"));
    BEZEL_EXPECT_FALSE(hasOwningElementError(r));
}

// §14.4: "interface types … OwnedArray<IConnection> is admitted."
BEZEL_TEST(owning_collection_element_761, owned_array_interface_admitted) {
    auto r = parseSemaAll(wrap("using var a := new OwnedArray<IFoo>();"));
    BEZEL_EXPECT_FALSE(hasOwningElementError(r));
}

// §14.4: "the instantiation rule checks the underlying type, not the nullable
// wrapper" — a nullable class element is admitted.
BEZEL_TEST(owning_collection_element_761, owned_array_nullable_class_admitted) {
    auto r = parseSemaAll(wrap("using var a := new OwnedArray<Foo?>();"));
    BEZEL_EXPECT_FALSE(hasOwningElementError(r));
}

// §28.11: an owning dictionary's KEY is value-like and unrestricted — a
// `string` KEY with a class VALUE is admitted.
BEZEL_TEST(owning_collection_element_761, owned_dictionary_string_key_class_value_admitted) {
    auto r = parseSemaAll(wrap("using var d := new OwnedDictionary<string, Foo>();"));
    BEZEL_EXPECT_FALSE(hasOwningElementError(r));
}

// §14.4: the BORROWING family is unrestricted — `Array<string>` is admitted
// (it stores borrows, not owned elements).
BEZEL_TEST(owning_collection_element_761, borrowing_array_string_admitted) {
    auto r = parseSemaAll(wrap("using var a := new Array<string>(); a.Add(\"x\");"));
    BEZEL_EXPECT_FALSE(hasOwningElementError(r));
}
