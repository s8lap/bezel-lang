// Spec-derived tests for §14.2 *Conditionally-admitted members* rule 1
// (proposal 0116 D7, issue #3330): a `where` clause on a member declaration
// may constrain only the member's own type parameters. A clause whose
// constrained identifier is a type parameter of the *enclosing* declaration
// rather than of the member itself is a compile error at the declaration.
//
// THE DEFECT (#3330, pre-fix): the form is grammatical (Appendix A.4's
// `MethodDecl` carries `GenericConstraints*` unconditionally), it passes
// the cycle/contradiction check (TypeResolution_Checks.cpp:851-864, which
// deliberately puts the enclosing parameters in scope so `where T: U` on a
// method of `class Box<U>` resolves), and SymbolCollection's
// `collectGenericParams` (src/sema/SymbolCollection.cpp:91-121) keys the
// `where` clauses onto a GenericParamSymbol by parameter NAME — finding no
// match for the enclosing type's T on a member that has no own T, the
// clause is silently dropped, never entering any constraint data structure.
// Nothing downstream enforces it. Per-instantiation constraint checking
// (Resolver::checkGenericConstraints at TypeResolution_Checks.cpp:6407)
// iterates only `tsym->genericParams` (the enclosing type's), never a
// MethodSymbol's. The constraint is parsed, scoped, cycle-checked, and
// discarded — the inert-looking-but-load-bearing annotation failure mode
// spec §9.2 names.
//
// THE FIX (#3330): checkGenericConstraintConsistency now reports a
// compile error at the method's declaration for every method-level `where`
// whose `typeParam` is not one of the method's OWN generic parameters.
// The diagnostic names §14.2 *Conditionally-admitted members* rule 1 and
// the offending identifier.
//
// TESTS:
//
//   W1 — REJECT (§14.2, #3330): `class Box<T>` declaring
//        `method Only() where T: IComparable<T>` is rejected — the clause
//        names Box's T, which is not Only's own type parameter (Only
//        introduces none). This is the issue body verbatim.
//
//   W2 — ACCEPT (control): the SAME method, dropping the offending clause,
//        is accepted. Pins that the fix rejects only the offending `where`
//        form, not bare methods in generic classes.
//
//   W3 — ACCEPT (control): `class Box<T>` declaring
//        `method Only<U>() where U: IComparable<U>` is accepted — U is the
//        method's own type parameter (the ordinary §14.2 clause form), even
//        though the bound names IComparable<U>. This is the form the spec
//        explicitly preserves per §14.2 rule 1 ("a clause naming the
//        member's own type parameter is the ordinary constraint form … and
//        is unaffected, including where its bound names an enclosing
//        parameter").
//
//   W4 — ACCEPT (control, §14.2 type-parameter-bound form): the BOUND may
//        name the enclosing parameter. `class Box<T>` declaring
//        `method Only<U>() where U: T` is accepted — U is Only's own
//        parameter, T is Box's, and `where U: T` is the §14.2 type-
//        parameter-bound form, fully defined. The D7 rule keys on the
//        constrained identifier (U), never on the bound.
//
//   W5 — ACCEPT (control, class-level `where`): `class Box<T> where T:
//        IComparable<T>` declaring `method Only() … end Only;` is accepted
//        — class-level clauses name the class's parameters and are
//        unaffected by D7. The class-level clause fires at every
//        instantiation of Box as a whole.
//
//   W6 — ACCEPT (control, two-parameter method): `class Box<T>` declaring
//        `method Only<U, V>() where U: class where V: IComparable<V>` is
//        accepted — both clauses name own parameters. The fix keys on each
//        clause independently.
//
//   W7 — REJECT (§14.2, multiple offending clauses): a method with TWO
//        `where` clauses, both naming enclosing parameters, is rejected
//        with exactly one diagnostic (the `unordered_set` deduplication
//        per identifier, NOT per clause — `where T: c1 where T: c2` would
//        still be one identifier `T`, but here we test the more common
//        `where T: c1 where U: c2` shape, two distinct identifiers).

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

bool hasDiagnosticContaining(const bezel::test::SemaResult& r,
                              std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// W1 (§14.2, #3330): the issue body's verbatim probe. `class Box<T>` declares
// `method Only() where T: IComparable<T>` — the clause names Box's T (Only
// introduces no own parameters). Pre-fix: parses, passes the cycle check,
// SymbolCollection drops it silently, the call is admitted. Post-fix:
// rejected at the declaration, naming the offending identifier and §14.2.
//
// The pre-fix program compiles AND admits `b.Only()` at a non-comparable
// `Box<Widget>`. The post-fix program does not compile at all. This test
// pins the REJECT — the diagnostic must fire.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_member_where_enclosing_3330,
           enclosing_param_clause_rejected_sec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "interface IComparable<T> "
        "  method CompareTo(other: T): int; "
        "end IComparable; "
        "class Box<T> "
        "  public method Only(): int where T: IComparable<T> "
        "    return 0; "
        "  end Only; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  return b.Only(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "method-level `where` clause names `T`"));
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "enclosing declaration"));
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "§14.2"));
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "Conditionally-admitted members"));
}

// ---------------------------------------------------------------------------
// W2 (control ACCEPT): the same method, NO `where` clause. The fix must not
// over-reject — bare methods in generic classes are the common case and
// must remain accepted.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_member_where_enclosing_3330,
           bare_method_in_generic_class_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public method Only(): int "
        "    return 0; "
        "  end Only; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  return b.Only(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W3 (control ACCEPT, §14.2 ordinary member-level `where`): `class Box<T>`
// declaring `method Only<U>() where U: SomeBound` — U is Only's OWN
// type parameter (the ordinary §14.2 clause form). Per spec §14.2 rule 1
// this is the unaffected side; the bound's identity doesn't matter to D7
// (which keys on the CONSTRAINED IDENTIFIER, never the bound), and we use
// a single-parameter class bound to keep the conformance check out of the
// picture — this test pins the *parser/scoper* (D7 does not over-reject
// own-param clauses), not the per-instantiation constraint arm.
//
// This is the negative control the issue body demands: "the method that
// introduces its own `T` and constrains it — that must keep working".
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_member_where_enclosing_3330,
           own_param_clause_accepted_sec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Box<T> "
        "  public method Only<U>(): int where U: Base "
        "    return 0; "
        "  end Only; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  return b.Only<Base>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W4 (control ACCEPT, §14.2 type-parameter-bound form): `class Box<T>`
// declaring `method Only<U>() where U: T` — U is the method's own
// parameter, T is the enclosing class's, and `where U: T` is the §14.2
// type-parameter-bound form ("a constraint whose bound is another type
// parameter — of the same declaration, or of the enclosing type — is
// grammatical and has subtype semantics"). The D7 rule keys on the
// CONSTRAINED IDENTIFIER (U), never on the bound (T).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_member_where_enclosing_3330,
           own_param_bound_to_enclosing_accepted_sec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public method Only<U>(): T where U: T "
        "    return default(T); "
        "  end Only; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  var x := b.Only<int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W5 (control ACCEPT, class-level `where`): `class Box<T> where T: Base` —
// the clause names the CLASS's T, not a member's. D7 rejects only
// member-level clauses; class-level clauses are unaffected and continue to
// be enforced at every instantiation of Box. (We use a non-generic class
// bound to keep the test orthogonal to the existing per-instantiation
// `where T: IComparable<T>` conformance check, which is its own area.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_member_where_enclosing_3330,
           class_level_where_clause_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Box<T> where T: Base "
        "  public method Only(): int "
        "    return 0; "
        "  end Only; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<Base>(); "
        "  return b.Only(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W6 (control ACCEPT, two-parameter method): both clauses name own
// parameters. The fix keys on each clause independently. (Uses non-
// generic bounds to keep the conformance check out of the test's
// scope — this pins the *scoper*, not the per-instantiation arm.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_member_where_enclosing_3330,
           two_param_method_both_clauses_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class BaseA end BaseA; "
        "class BaseB end BaseB; "
        "class Box<T> "
        "  public method Only<U, V>(): int "
        "    where U: BaseA "
        "    where V: BaseB "
        "    return 0; "
        "  end Only; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  return b.Only<BaseA, BaseB>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W7 (REJECT, two distinct offending identifiers): a method with two
// `where` clauses naming two different enclosing parameters
// (`where T: c1 where U: c2`, on `class Pair<T, U>`). Each offending
// identifier gets its own diagnostic — but the test only pins that
// AT LEAST one diagnostic fires; the dedup is per-identifier, not per-
// clause, so `where T: c1 where T: c2` (same identifier twice) would
// produce exactly one diagnostic. Either way the program is rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_member_where_enclosing_3330,
           two_distinct_enclosing_param_clauses_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<T, U> "
        "  public method Only(): int "
        "    where T: class "
        "    where U: class "
        "    return 0; "
        "  end Only; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p := new Pair<int, int>(); "
        "  return p.Only(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "method-level `where` clause names"));
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "enclosing declaration"));
}