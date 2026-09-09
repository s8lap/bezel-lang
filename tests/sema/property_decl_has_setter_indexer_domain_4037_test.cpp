// Spec-derived tests for #4037 — §12.1 / §12.5, v0.295.
//
// `TypeResolution_Checks.cpp`'s `propertyDeclHasSetter` re-derives the
// "is this Auto-bodied PropertyDecl the §12.5 abstract shorthand" answer
// with its own inline modifier scan instead of calling the shared
// `sema::isAbstractPropertyShorthand` (sema/Symbol.hpp). #4037 found the
// duplication (LANE_RULES §6) but confirmed no live behavioral bug from it
// — and confirmed the two spellings are NOT safely interchangeable: a naive
// `if (isAbstractPropertyShorthand(pd)) return false;` substitution would
// silently flip an abstract INDEXER shorthand from correctly get-only to
// (incorrectly) read-write, because `isAbstractPropertyShorthand`
// unconditionally excludes indexers (`p.isIndexer` guard — its job is
// storage classification, where indexers are excluded because they never
// get storage regardless of `abstract`-ness; a different question from
// "does this declaration's shape carry a setter").
//
// This fix is comment-only (documents why `propertyDeclHasSetter` cannot
// delegate, at the call site itself) — no behavior changes. These tests pin
// the CURRENT correct behavior on both domains (named property AND
// indexer) so a future refactor that performs the naive substitution fails
// the indexer rows here instead of shipping silently.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// ACCEPT — abstract INDEXER shorthand, correctly overridden get-only. The
// base's `abstract property [i: int]: int;` is get-only by the §12.5
// shorthand convention; a get-only override matches.
BEZEL_TEST(sema_property_decl_has_setter_indexer_domain_4037,
           abstract_indexer_shorthand_getonly_override_accepted_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseIdx4037 "
        "  public abstract property [i: int]: int; "
        "end BaseIdx4037; "
        "class DerivedIdx4037 : BaseIdx4037 "
        "  private _v: int := 42; "
        "  public override property [i: int]: int "
        "    get => _v; "
        "  end; "
        "end DerivedIdx4037; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DerivedIdx4037(); "
        "    return d[0]; "
        "  end Main; "
        "end C;"),
        42);
}

// REJECT — abstract INDEXER shorthand, override ADDS a setter the get-only
// base lacks. Must be rejected on the accessor-presence mismatch (§12.1),
// exactly like the named-property control below — this is the row that
// would silently flip to ACCEPT under a naive `isAbstractPropertyShorthand`
// substitution (that predicate answers `false` for an indexer, so the
// abstract-shorthand get-only exclusion would never fire for it).
BEZEL_TEST(sema_property_decl_has_setter_indexer_domain_4037,
           abstract_indexer_shorthand_added_setter_override_rejected_spec_12_1) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseIdxSet4037 "
        "  public abstract property [i: int]: int; "
        "end BaseIdxSet4037; "
        "class DerivedIdxSet4037 : BaseIdxSet4037 "
        "  private _v: int := 42; "
        "  public override property [i: int]: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end DerivedIdxSet4037; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DerivedIdxSet4037(); "
        "    return d[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "does not match the base member's accessor set/get presence"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.1"));
}

// ACCEPT control — the identical shape on a NAMED property (non-indexer):
// abstract shorthand get-only, correctly overridden get-only. Confirms the
// named-property domain (where `isAbstractPropertyShorthand` DOES answer
// meaningfully) is unaffected.
BEZEL_TEST(sema_property_decl_has_setter_indexer_domain_4037,
           abstract_named_property_getonly_override_accepted_control_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseNamed4037 "
        "  public abstract property Val: int; "
        "end BaseNamed4037; "
        "class DerivedNamed4037 : BaseNamed4037 "
        "  private _v: int := 7; "
        "  public override property Val: int "
        "    get => _v; "
        "  end; "
        "end DerivedNamed4037; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DerivedNamed4037(); "
        "    return d.Val; "
        "  end Main; "
        "end C;"),
        7);
}

// REJECT control — the identical added-setter mismatch on a NAMED property.
// Mirrors tests/lit/sema/sema_prop_override_1614__added_setter_diagnoses.bzl
// (a `virtual` expression-bodied base, not the `abstract` Auto shorthand);
// this row is the abstract-shorthand-specific analogue.
BEZEL_TEST(sema_property_decl_has_setter_indexer_domain_4037,
           abstract_named_property_added_setter_override_rejected_control_spec_12_1) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseNamedSet4037 "
        "  public abstract property Val: int; "
        "end BaseNamedSet4037; "
        "class DerivedNamedSet4037 : BaseNamedSet4037 "
        "  private _v: int := 7; "
        "  public override property Val: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end DerivedNamedSet4037; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DerivedNamedSet4037(); "
        "    return d.Val; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "does not match the base member's accessor set/get presence"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.1"));
}
