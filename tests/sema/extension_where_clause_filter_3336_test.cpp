// Spec-derived tests for §26.4 (v0.277, issue #3336) — block-level `where`
// clause as an applicability filter.
//
// §26.4: "A `where` clause on the block constrains which instantiations of
// the extended type the extension applies to." §26.5.1 sharpens: "A `where`
// clause is an *applicability* filter, never a ranking signal."
//
// THE DEFECT (#3336, pre-fix): ExprTC::lookupExtensionMember's candidate
// collection in src/sema/ExprTypecheck.cpp:6937 read `decl->genericConstraints`
// in exactly two places — :6994 to COLLECT free variables for target matching
// (so `extension IEnumerable<T>` and `extension IEnumerable<T> where T: class`
// were treated IDENTICALLY), and :7181 to SPELL the clause in the rule-4
// ambiguity diagnostic. NO candidate was ever discarded for FAILING its `where`
// clause. The §5.1 stdlib case (`public extension IEnumerable<T> where T:
// IComparable<T>` at stdlib §5.1) is the canonical instance — a hand-written
// gate in `ExprTypecheck_Calls.cpp:5249-5295` enforces the same clause the
// extension declares, because the compiler's own applicability filter was a
// silent no-op.
//
// THE FIX (#3336): a `where T: c1, c2` clause is now evaluated against the
// receiver's instantiation via ExprTC::extWhereClauseProvablyViolated, and a
// provable violation drops the entire block from the candidate set. The
// check is intentionally SILENT (§26.5.1 calls it an "applicability filter",
// not a diagnostic site) — the existing "no such member" diagnostic from the
// caller (resolveMemberAccess) carries the user-facing message when ALL
// candidates are dropped this way.
//
// TESTS:
//
//   W1 — REJECT: an extension whose block-level `where` clause is unsatisfied
//        at the receiver's instantiation is NOT a candidate — the call site
//        emits a "no such member" diagnostic. This is the §26.4 probe from
//        the issue body (a Widget that doesn't implement IComparable<Widget>
//        over an `extension Array<T> where T: IComparable<T>`).
//
//   W2 — ACCEPT (control): the SAME extension applied to a SATISFYING
//        instantiation resolves cleanly — the fix must not over-reject.
//
//   W3 — ACCEPT (control): a primitive substitution `int` (which implements
//        IComparable<int> via the stdlib's compiler-provided conformance)
//        keeps the constraint — this is the §14.2 "primitives conform to
//        IComparable<T>" concession, and the lookup's `decided` out-parameter
//        must NOT decide it as a violation for primitives.
//
//   W4 — DISJOINT CLAUSES, §26.5.1: two extensions over the SAME head with
//        DISJOINT `where` clauses resolve cleanly at the matching
//        instantiation each, with NO ambiguity error. The pre-fix code kept
//        BOTH candidates (because it never filtered any), and the §26.5
//        rule-4 ambiguity path fired. After the fix, each instantiation
//        leaves exactly one surviving candidate — the §26.5.1
//        "applicability filter" wording is what makes this case unambiguous
//        rather than ambiguous.
//
//   W5 — ACCEPT (control): a non-generic extension (no block-level `where`
//        clause) is unaffected by the filter — the predicate's loop is empty
//        and the candidate is preserved.
//
//   W6 — REJECT, class-bound filter: `extension Bag<T> where T: class`
//        applied to a value-type receiver (e.g. `int`) is dropped — the
//        `class` arm of extConstraintItemProvablyViolated fires for the
//        non-nullable value-typed arg.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
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
// W1 (§26.4, #3336): the issue's probe. `extension Array<T> where T:
// IComparable<T>` declares `Peak`; an `Array<Widget>` receiver with Widget
// implementing NO IComparable must be rejected — the clause filters the
// extension OUT of the candidate set, so `Peak` is a "no such member" from
// the call site's existing diagnostic path.
//
// Before the fix: the §26.4 clause was inert, lookupExtensionMember
// returned the extension method, and the call COMPILED — silently
// resolving to a member that the spec says doesn't apply.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_where_clause_filter_3336,
           non_satisfying_receiver_drops_block_rejected) {
    auto r = parseSemaAll(
        "module M; "
        // §26.4's worked-example clause: a `where T: IComparable<T>` block
        // admits only T that conform to IComparable<T>.
        "extension Array<T> where T: IComparable<T> "
        "  public method Peak(): int => 42; "
        "end; "
        // Widget declares no CompareTo — must NOT conform.
        "class Widget public method Tag(): int => 1; end Widget; "
        "class C public static method Main(): int "
        "  var w := new Array<Widget>(); "
        "  return w.Peak(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The lookup is silent (no §26.4-specific diagnostic); the existing
    // "no member" reject from resolveMemberAccess carries the message. The
    // §26.5 step 1 instance-wins rule does not apply (Array has no `Peak`),
    // so the lookup visits extensions and finds zero after the fix filters
    // the only candidate out.
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, "no member"));
}

// ---------------------------------------------------------------------------
// W2 (control ACCEPT, §26.4, #3336): the SAME extension applied to a
// SATISFYING `Array<Box>` (Box implements IComparable<Box>) resolves
// cleanly. The fix must not over-reject a class that legitimately
// conforms.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_where_clause_filter_3336,
           satisfying_receiver_keeps_block_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "extension Array<T> where T: IComparable<T> "
        "  public method Peak(): int => 42; "
        "end; "
        "class Box: IComparable<Box> "
        "  public method CompareTo(other: Box): int => 0; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Array<Box>(); "
        "  return b.Peak(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W3 (control ACCEPT, §14.2 / §3.6 primitives): `Array<int>` substitutes T =
// int, where int is a primitive that implements IComparable<int> via the
// stdlib's compiler-provided conformance (the §11.8 rule that numeric
// primitives conform to IComparable<T>). The fix's predicate DEFERs the
// `type` check on a primitive against a builtin bound
// (groundSatisfiesTypeBound's primitive arm — `decided` stays false → keep),
// so this control pins that the primitive-conformance shortcut is preserved
// — the lookup must NOT decide the constraint as violated here, or every
// `extension Array<T> where T: IComparable<T>` over a primitive element
// would be broken.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_where_clause_filter_3336,
           primitive_int_receiver_keeps_block_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "extension Array<T> where T: IComparable<T> "
        "  public method Peak(): int => 42; "
        "end; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(); "
        "  return a.Peak(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W4 (§26.5.1, #3336): two extensions over the SAME head with DISJOINT
// `where` clauses. `extension Array<T> where T: class` admits only reference
// types; `extension Array<T> where T: value` admits only value types. They
// never BOTH apply at one instantiation — one survives and the other is
// filtered out by the new applicability check.
//
// Before the fix: BOTH candidates survived (the `where` clause was inert),
// the §26.5 rule-4 ambiguity path fired (they share the same substituted
// head `Array<int>` / `Array<string>`), and the user saw an "ambiguous
// extension member" diagnostic. After the fix, exactly one survives at each
// instantiation and the call resolves cleanly.
//
// Each test below names the receiver's instantiating type and the
// expected-to-be-selected member's body signature (1 for the `class`-only
// block, 100 for the `value`-only block).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_where_clause_filter_3336,
           disjoint_clauses_resolve_at_class_instantiation) {
    auto r = parseSemaAll(
        "module M; "
        "extension Array<T> where T: class "
        "  public method Pick(): int => 1; "
        "end; "
        "extension Array<T> where T: value "
        "  public method Pick(): int => 100; "
        "end; "
        "class Box public method Tag(): int => 0; end Box; "
        "class C public static method Main(): int "
        "  var b := new Array<Box>(); "
        "  return b.Pick(); "
        "end Main; end C;");
    // No error: T = Box is a class, so the `where T: class` block is the
    // sole survivor; the `where T: value` block is dropped by the filter.
    // The `where T: value` candidate is now NOT in the candidate set, so
    // §26.5 rule-4 (which requires at least 2 surviving candidates) does
    // not fire — the lookup resolves to the class-block's Pick() = 1.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_extension_where_clause_filter_3336,
           disjoint_clauses_resolve_at_value_instantiation) {
    auto r = parseSemaAll(
        "module M; "
        "extension Array<T> where T: class "
        "  public method Pick(): int => 1; "
        "end; "
        "extension Array<T> where T: value "
        "  public method Pick(): int => 100; "
        "end; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(); "
        "  return a.Pick(); "
        "end Main; end C;");
    // T = int is a value type, so the `where T: value` block is the sole
    // survivor; the `where T: class` block is dropped by the filter.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W5 (control ACCEPT, §26.5.1, #3336): a NON-GENERIC extension (no block-
// level `where` clause) is unaffected by the filter — the loop is empty
// and the candidate is preserved. The fix must not have introduced a
// regression in the common case of plain `extension Foo`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_where_clause_filter_3336,
           unconstrained_block_unaffected_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public _n: int := 0; end Widget; "
        "extension Widget "
        "  public method Describe(): int => this._n + 1; "
        "end; "
        "class C public static method Main(w: Widget): int "
        "  return w.Describe(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// W6 (§26.4 / §14.2, #3336): a `where T: class` clause applied to a
// value-type receiver is dropped. The `class` arm of
// extConstraintItemProvablyViolated fires for the non-nullable value-typed
// arg (groundCategorize returns Value for int / records / enums). This is
// the §14.2 *table row* for `class` and a good exercise of the non-type
// arm of the predicate (W1..W4 exercise only the `type` arm).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_where_clause_filter_3336,
           where_class_filter_drops_value_type_receiver) {
    auto r = parseSemaAll(
        "module M; "
        "extension Array<T> where T: class "
        "  public method Pick(): int => 1; "
        "end; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(); "
        "  return a.Pick(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, "no member"));
}

// ---------------------------------------------------------------------------
// W7 (control ACCEPT, §26.4, #3336): the §5.1 stdlib shape
// `extension IEnumerable<T> where T: IComparable<T>` resolving over an
// `Array<int>` (an IEnumerable<int> ancestor) is a regression pin for
// `#714` — the extension has a `where T: IComparable<T>` clause that the
// primitive substitution satisfies via the §14.2 / §11.8 primitive
// conformance shortcut. Before the fix this was the only the §5.1 case
// tested; after the fix, this test pins that the new filter does not
// disturb the existing accepted path on a primitive receiver.
//
// (Mirrors the #714 / `generic_extension_714_test.cpp` control family, but
// scoped to the §26.4 semantic — the block clause is enforced — rather
// than the resolution path itself.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_where_clause_filter_3336,
           ienumerable_icomparable_over_array_int_accepted_3336) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> where T: IComparable<T> "
        "  public method MyCount(): int "
        "    var n := 0; "
        "    for var item in this do n := n + 1; end; "
        "    return n; "
        "  end MyCount; "
        "end; "
        "public static class P public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.MyCount(); "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
