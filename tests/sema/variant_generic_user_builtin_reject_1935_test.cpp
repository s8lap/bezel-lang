// Spec-derived tests for §11 / §12.9 / §5.1 (#1935, spec v0.196): a USER-declared
// type is assignable to a BUILTIN generic interface ONLY when it nominally
// implements that interface (directly or transitively). Interface conformance in
// Bezel is nominal (§11) — a type conforms only by listing the interface among
// its bases — so a builtin interface absent from the source's declared-base
// closure is PROOF the source does not conform, and the assignment is a §5.1
// error.
//
// The unsound ACCEPT this pins: `variantGenericAssignable` took a conservative
// "user-declared → builtin" residual accept without ever checking base
// conformance, so an unrelated `MyBag<Dog>` (which implements nothing) was
// accepted as `IList<Dog>`. A later interface call (`x.Insert(...)`) would then
// dispatch through an itable the object lacks — runtime type confusion, the
// worst class of hole (an ill-typed program admitted). #1851 fixed only the
// both-builtin no-candidate case and explicitly left this residual open.
//
// The rejection must fire ONLY where non-conformance is PROVABLE — the target is
// a fully-modeled borrowing collection interface AND every builtin base along the
// source's declared-base closure is likewise fully modeled (so absence is real,
// not an unmodeled gap). A source that legitimately implements the target
// (directly, via a transitive builtin super such as IList<T> : … : IEnumerable<T>,
// or through a user-declared intermediate) must stay ACCEPTED — no over-rejection
// of §12.9 conformance.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff an ERROR diagnostic is the §5.1 non-conversion — pins THIS defect
// (the assignment was rejected as not implicitly convertible), not merely "some
// error".
bool hasConversionError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---- FAIL-without / PASS-with: non-conforming sources are REJECTED ----

// (1) The issue's canonical repro: `MyBag<T>` implements NOTHING, assigned to
// `IList<Dog>`. FAILS WITHOUT the fix (variantGenericAssignable's user→builtin
// residual conservatively accepted). PASSES WITH it (base conformance verified,
// no IList in MyBag's closure → §5.1 reject).
BEZEL_TEST(variant_generic_user_builtin_reject_1935, unrelated_generic_to_ilist_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "class MyBag<T> end MyBag;\n"
        "class C\n"
        "  public method M(a: MyBag<Dog>)\n"
        "    var x: IList<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (2) Same unrelated source assigned to a builtin SUPER `IEnumerable<Dog>` —
// still not in MyBag's (empty) closure, so rejected.
BEZEL_TEST(variant_generic_user_builtin_reject_1935, unrelated_generic_to_ienumerable_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "class MyBag<T> end MyBag;\n"
        "class C\n"
        "  public method M(a: MyBag<Dog>)\n"
        "    var x: IEnumerable<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (3) A SIBLING mismatch: the source implements `ICollection<Dog>` but is
// assigned to `IList<Dog>`. IList<T> : ICollection<T> (not the reverse), so an
// ICollection is not an IList — the walk from ICollection reaches only
// IReadOnlyCollection/IEnumerable, never IList → rejected.
BEZEL_TEST(variant_generic_user_builtin_reject_1935, icollection_to_ilist_sibling_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "interface IMyColl<T>: ICollection<T> end IMyColl;\n"
        "class C\n"
        "  public method M(a: IMyColl<Dog>)\n"
        "    var x: IList<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// ---- Regression guards: legitimate conformance stays ACCEPTED (no over-reject) ----

// (4) A user interface that DIRECTLY extends the builtin `IList<T>` is assignable
// to `IList<Dog>`. The builtin base survives only on the syntactic AST base list
// (it has no TypeSymbol) — the walk must recover it there.
BEZEL_TEST(variant_generic_user_builtin_reject_1935, user_iface_extends_ilist_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "interface IMyList<T>: IList<T> end IMyList;\n"
        "class C\n"
        "  public method M(a: IMyList<Dog>)\n"
        "    var x: IList<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (5) The same source assigned to a TRANSITIVE builtin super `IEnumerable<Dog>`
// (IList<T> : IReadOnlyList<T> : … : IEnumerable<T>) — the builtin-hierarchy
// expansion must reach it, so the assignment is accepted.
BEZEL_TEST(variant_generic_user_builtin_reject_1935, user_iface_to_transitive_ienumerable_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "interface IMyList<T>: IList<T> end IMyList;\n"
        "class C\n"
        "  public method M(a: IMyList<Dog>)\n"
        "    var x: IEnumerable<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) Conformance through a USER-declared intermediate: a class extends a user
// interface which extends the builtin `IList<T>`. Both `IList<Dog>` and its
// builtin super `IEnumerable<Dog>` must be reachable and accepted.
BEZEL_TEST(variant_generic_user_builtin_reject_1935, user_class_multihop_to_ilist_and_ienumerable_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "interface IMyList<T>: IList<T> end IMyList;\n"
        "class MyList<T>: IMyList<T> end MyList;\n"
        "class C\n"
        "  public method M(a: MyList<Dog>)\n"
        "    var x: IList<Dog> := a;\n"
        "    var y: IEnumerable<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) Broad guard: a class that genuinely implements the DECLARED interface
// `IComparable<Box>` stays assignable to it (this travels a different — Named-
// Named — assignability branch, so it must be unaffected by the #1935 change).
BEZEL_TEST(variant_generic_user_builtin_reject_1935, class_implements_icomparable_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Box(v: int): IComparable<Box>\n"
        "  public method CompareTo(other: Box): int\n"
        "    return 0;\n"
        "  end CompareTo;\n"
        "end Box;\n"
        "class C\n"
        "  public method M(b: Box)\n"
        "    var c: IComparable<Box> := b;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
