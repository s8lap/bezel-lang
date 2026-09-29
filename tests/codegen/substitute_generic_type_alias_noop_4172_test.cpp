// Spec-derived regression test for #4172 — generic type-alias substitution is a
// semantic no-op (§14.1, §27.8, v0.298):
//
// A type alias `type Alias<T> = R<T>` (where R is a record) is interchangeable
// with its aliased type at every instantiation point: `Alias<int>` and `R<int>`
// are the same TypeRef after substitution. The unified walker (fix #4212,
// commit 7d98c260) guarantees all three call sites agree on the same
// substitution authority; this test pins the interchangeability property that
// §27.8 asserts for every alias instantiation.
//
// NOTE: `Array<T>` and other built-in collection types cannot be used here
// because `new AliasArray<int>()` where `type AliasArray<T> := Array<T>`
// fails with an internal compiler error (#2434).  Record-based aliases work
// correctly and demonstrate the same no-op property.
//
// §14.1 "Generic types" (v0.298): every bare type-parameter reference inside a
// generic body substitutes to the concrete instantiation argument.
//
// §27.8 "Type aliases" (v0.298): "Each instantiation of the alias is
// interchangeable with the corresponding instantiation of the aliased type."  A
// generic alias referenced with `<int>` is byte-identical to the aliased type
// referenced with `<int>` after the unified walker threads substitution through
// the alias reference site.
//
// Fix #4172 regression test (Family I1 member row).
// Pre-fix reference: 7d98c260 hoist, a8cd1920 oracle strengthening,
// fe1a362e unified walker test.

#include "support/aot_support.hpp"         // runAot
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

// ---------------------------------------------------------------------------
// §27.8 — Type-alias no-op, generic function return: a generic function
// `Build<U>(): AliasR<U>` must return the same TypeRef as `R<U>`.  If the
// no-op is broken, the return type of `Build<int>()` is not `R<int>` and
// the assignment to `var y: R<int>` fails with a type-conversion error.
//
// Exercises the `Resolver::substituteAliasGenericArgs` caller path.
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_type_alias_noop_4172,
           type_alias_noop_generic_function_return_spec_27_8) {
    // Row 1: generic type alias used as function return type.  The function
    // `Build<U>(): AliasR4172<U>` is semantically `Build<U>(): R4172<U>`.  A
    // call to `Build<int>()` can be assigned to a variable typed as `R4217<int>`.
    const std::string prog =
        "module M; "
        "record R4217<T>(V: T); "
        "type AliasR4217<T> := R4217<T>; "
        "public static class C "
        "  public static method Build<U>(): AliasR4217<U> "
        "    return new R4217<U>(default(U)); "
        "  end Build; "
        "  public static method Main(): int "
        "    var x: AliasR4217<int> := C.Build<int>(); "
        "    var y: R4217<int> := C.Build<int>(); " // alias is no-op — same as R4217<int>
        "    if x.V = 0 and y.V = 0 then return 42; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// §27.8 / §14.1 — Type-alias no-op, two-level nesting: `OuterAlias<U>` ->
// `InnerAlias<U>` -> `R<U>`.  After full expansion `OuterAlias<int>` is
// `R<int>`.  A one-level miss (outer expanded, inner left as bare T) would
// produce a type with no `.V` member and the program would be rejected.
//
// The transitive substitution through both alias levels is the key property:
// the unified walker must thread substitution through every alias reference site.
// ---------------------------------------------------------------------------

BEZEL_TEST(substitute_generic_type_alias_noop_4172,
           type_alias_noop_nested_two_levels_spec_27_8_14_1) {
    // Row 2: two-level alias nesting — OuterAlias4217<U> := InnerAlias4217<U> :=
    // R4217<U>.  After full expansion `OuterAlias4217<int>` is `R4217<int>`.
    const std::string prog =
        "module M; "
        "record R4217<T>(V: T); "
        "type InnerAlias4217<T> := R4217<T>; "
        "type OuterAlias4217<U> := InnerAlias4217<U>; "
        "public static class C "
        "  public static method Main(): int "
        "    var r: OuterAlias4217<int> := new R4217<int>(77); "
        "    if r.V = 77 then return 77; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 77);
}
