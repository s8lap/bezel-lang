// Spec-derived sema tests for issue #1579 (v0.182) — §14.2 "Constraints on
// overrides and interface implementations":
//
//   "An `override` of a generic method ... inherits the overridden ...
//    declaration's constraints. Omitting the `where` clauses is the
//    recommended spelling and means exactly the inherited set; the clauses
//    may instead be restated in an equivalent form — the same constraint set
//    per type parameter, compared order-insensitively and after type-alias
//    resolution ([§27.8]): `where T: IFoo` and `where T: AliasOfIFoo`
//    restate the same set — for readability. A **differing** clause — added,
//    removed, narrowed, or widened relative to the inherited set — is a
//    compile error naming the inherited constraints: dispatch flows through
//    the base or interface signature, so its constraints are the only ones
//    call sites are ever checked against — an *added* constraint on the
//    override would go unenforced at every virtual call (unsound), and a
//    *weakened* one would advertise admissions the dispatch surface never
//    grants."
//
// This covers the CLASS-OVERRIDE path (checkOverrideSignatureMatch); a
// method implementing a generic INTERFACE member is a separate, untouched
// code path per the fix's scope note.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some emitted diagnostic is the §14.2 override-constraint-mismatch
// error: it names the restated `where` clause and cites §14.2.
bool hasOverrideConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("restates a `where") == std::string::npos)
            continue;
        if (d.message.find("\xC2\xA7""14.2") == std::string::npos) continue;
        return true;
    }
    return false;
}

const char* kIFoo = "interface IFoo end IFoo; ";
const char* kIBar = "interface IBar end IBar; ";

}  // namespace

// ===========================================================================
// POSITIVE — a differing restated clause is rejected.
// ===========================================================================

// §14.2: the override names a DIFFERENT interface bound (IBar instead of the
// base's IFoo) — rejected.
BEZEL_TEST(override_generic_constraint_inheritance_1579, differing_interface_bound_rejected) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIFoo + kIBar +
        "class Base "
        "  public virtual method Store<T>(x: T) where T: IFoo end Store; "
        "end Base; "
        "class Derived: Base "
        "  public override method Store<T>(x: T) where T: IBar end Store; "
        "end Derived; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasOverrideConstraintError(r));
}

// §14.2: the override ADDS a constraint (`class`) the base did not declare —
// an added constraint would go unenforced at every virtual call — rejected.
BEZEL_TEST(override_generic_constraint_inheritance_1579, added_constraint_rejected) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIFoo +
        "class Base "
        "  public virtual method Store<T>(x: T) where T: IFoo end Store; "
        "end Base; "
        "class Derived: Base "
        "  public override method Store<T>(x: T) where T: IFoo, class "
        "  end Store; "
        "end Derived; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasOverrideConstraintError(r));
}

// §14.2: the override REMOVES the base's constraint entirely (bare `where
// T: notnull` narrowed away from `IFoo`, a widened admission relative to the
// dispatch surface's checked constraint) — rejected.
BEZEL_TEST(override_generic_constraint_inheritance_1579, narrowed_constraint_rejected) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIFoo +
        "class Base "
        "  public virtual method Store<T>(x: T) where T: IFoo end Store; "
        "end Base; "
        "class Derived: Base "
        "  public override method Store<T>(x: T) where T: notnull "
        "  end Store; "
        "end Derived; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasOverrideConstraintError(r));
}

// ===========================================================================
// NEGATIVE — omitted / equivalent-reordered / alias-resolved restatements
// compile cleanly (no §14.2 override-constraint error).
// ===========================================================================

// §14.2: omitting the `where` clause on the override is the recommended
// spelling — inherits the base's constraint set exactly, no error.
BEZEL_TEST(override_generic_constraint_inheritance_1579, omitted_clause_inherits_ok) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIFoo +
        "class Base "
        "  public virtual method Store<T>(x: T) where T: IFoo end Store; "
        "end Base; "
        "class Derived: Base "
        "  public override method Store<T>(x: T) end Store; "
        "end Derived; ");
    BEZEL_EXPECT_FALSE(hasOverrideConstraintError(r));
}

// §14.2: restating the IDENTICAL single-item clause compiles.
BEZEL_TEST(override_generic_constraint_inheritance_1579, identical_clause_ok) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIFoo +
        "class Base "
        "  public virtual method Store<T>(x: T) where T: IFoo end Store; "
        "end Base; "
        "class Derived: Base "
        "  public override method Store<T>(x: T) where T: IFoo end Store; "
        "end Derived; ");
    BEZEL_EXPECT_FALSE(hasOverrideConstraintError(r));
}

// §14.2: restating the same TWO-item constraint set in REVERSED order
// (`class, IFoo` vs the base's `IFoo, class`) is the SAME set, compared
// order-insensitively — compiles.
BEZEL_TEST(override_generic_constraint_inheritance_1579, reordered_clause_ok) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIFoo +
        "class Base "
        "  public virtual method Store<T>(x: T) where T: IFoo, class "
        "  end Store; "
        "end Base; "
        "class Derived: Base "
        "  public override method Store<T>(x: T) where T: class, IFoo "
        "  end Store; "
        "end Derived; ");
    BEZEL_EXPECT_FALSE(hasOverrideConstraintError(r));
}

// §14.2 / §27.8: restating the base's bound through a TYPE ALIAS of the same
// interface (`AliasOfIFoo` aliasing `IFoo`) is the same set after
// alias-resolution — compiles.
BEZEL_TEST(override_generic_constraint_inheritance_1579, alias_resolved_clause_ok) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIFoo +
        "type AliasOfIFoo := IFoo; "
        "class Base "
        "  public virtual method Store<T>(x: T) where T: IFoo end Store; "
        "end Base; "
        "class Derived: Base "
        "  public override method Store<T>(x: T) where T: AliasOfIFoo "
        "  end Store; "
        "end Derived; ");
    BEZEL_EXPECT_FALSE(hasOverrideConstraintError(r));
}

// §14.2: a THIRD-level override in a chain (Derived2 : Derived1 : Base) that
// also omits the clause inherits the same base constraint transitively — no
// error at any level.
BEZEL_TEST(override_generic_constraint_inheritance_1579, chained_omitted_clauses_ok) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIFoo +
        "class Base "
        "  public virtual method Store<T>(x: T) where T: IFoo end Store; "
        "end Base; "
        "class Mid: Base "
        "  public override method Store<T>(x: T) end Store; "
        "end Mid; "
        "class Leaf: Mid "
        "  public override method Store<T>(x: T) end Store; "
        "end Leaf; ");
    BEZEL_EXPECT_FALSE(hasOverrideConstraintError(r));
}
