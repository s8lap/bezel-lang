// Spec-derived sema tests for fix #3271 (spec §14.6, v0.262): expanding
// in-SCC instantiation edges are a compile error at the declaration holding
// the edge, with a diagnostic naming the cycle and the growing argument.
//
// The instantiation-request graph has the program's generic types and generic
// members as nodes (§14.6: each generic type is a node, each generic member
// is its own node; a non-generic member's edges attach to its declaring
// type's node). Edges arise from generic calls, generic-type instantiations,
// base-clause references, and field, choice-payload, and signature types;
// override bodies contribute their edges to the overridden member's node.
// Within each strongly-connected component, every edge must substitute each
// target parameter with a bare type parameter in scope at the source
// declaration (the source's own, or an enclosing declaration's) OR a closed
// type (containing no type parameter). Any other substitution — an argument
// containing an in-scope type parameter other than as the bare argument
// itself, the nullable form `T?` included — is the compile error.
//
// The two reproducing shapes (#0104 §4.2):
//   - call route: `class F<T>` with `method M(): F<Pair<T, T>>` — the
//     method's body requests F at a constructed argument containing T;
//   - data route: `class Node<T>` with `owned next: Node<Pair<T, T>>?` —
//     a field's type names Node at a constructed argument containing T.
//
// Pre-fix state (per #0104 §4.2): the cache-before-emit registration
// (`specializedMethods_[cacheKey] = specFn`) terminates only same-key
// recursion. The expanding chain (`F<T>` requesting `F<Pair<T, T>>`) mints
// a fresh key per step and never terminates. Sema neither rejects nor
// terminates on the shape. This fix closes the gap at declaration time.
//
// Citations: spec §14.6 (v0.262); proposal 0104 §4.3 (installed-base sweep:
// zero expanding self-edges across the repo's 657 generic declarations, so
// no migration expected); #3270 ruling; #3271 implementation tracker.
//
// Done means:
//   - the declaration-site rejection fires on the call route
//     (`F<T>` with `M(): F<Pair<T, T>>`);
//   - it fires on the data route
//     (`class Node<T>` with field `next: Node<Pair<T, T>>?`);
//   - the diagnostic names the cycle and the growing argument, cites §14.6
//     and #3271, and lands at the DECLARATION holding the edge (per the
//     spec's declaration-site shape, paralleling §14.2's bound cycles and
//     §27.8's alias rule);
//   - the legal shapes still compile: same-key recursion
//     (`F<T>` with `M(): F<T>`), bare-parameter permutation cycles
//     (`F<T, U>` with `M(): F<U, T>`), a generic member's edge to its
//     enclosing type at the enclosing parameters, and a `T?` substitution
//     on a CROSS-SCC edge (where the rule does not apply);
//   - the nullable `T?` arm IS rejected inside the SCC per spec text
//     ("the nullable form T? included").

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

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// §14.6 / #3271: HEADLINE REJECTIONS — the two reproducing shapes.
// =========================================================================

// CALL ROUTE — the v0.262 spec example. `class F<T>` declaring a method
// whose return type names `F<Pair<T, T>>`. The method's body requests
// `F<...>` at an argument that contains T (the source's own parameter,
// not bare). The source is F (the declaring type's node, since the method
// is non-generic), the target is F (the same), the substitution
// `T → Pair<T, T>` substitutes the target's T with a constructed type
// containing the source's T. SCC = {F}; the self-loop is the in-SCC
// edge; the substitution is neither bare (it's `Pair<T, T>`) nor closed
// (it contains T). REJECTED at the declaration.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           call_route_returning_self_at_constructed_arg_rejected_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class F<T> "
        "  public method M(): F<Pair<T, T>> "
        "    return new F<Pair<T, T>>(); "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3271"));
    // The diagnostic names the cycle (F) and the growing argument.
    BEZEL_EXPECT_TRUE(hasMsg(r, "F"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "Pair<T, T>"));
}

// DATA ROUTE — the second reproducing shape from #0104 §4.2. `class
// Node<T>` with a field of type `Node<Pair<T, T>>?`. The field type
// names `Node` at an argument that contains T. The source is Node
// (the declaring type's node), the target is Node (the same), the
// substitution `T → Pair<T, T>` substitutes the target's T with a
// constructed type containing the source's T. SCC = {Node}; the
// self-loop is the in-SCC edge. REJECTED at the declaration.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           data_route_field_self_at_constructed_arg_rejected_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Node<T> "
        "  public owned next: Node<Pair<T, T>>?; "
        "  public static method Main(): int return 0; end Main; "
        "end Node;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3271"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "Node"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "Pair<T, T>"));
}

// NULLABLE FORM on the data route — `Node<Pair<T, T>?>?` — the nullable
// form IS rejected per spec ("the nullable form T? included"). The
// substitution is `T → Pair<T, T>?`; this is neither bare (it's `Pair<T,
// T>?`, constructed with a nullable form) nor closed (Pair contains T).
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           data_route_nullable_form_rejected_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Node<T> "
        "  public owned next: Node<Pair<T, T>?>?; "
        "  public static method Main(): int return 0; end Main; "
        "end Node;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§14.6"));
    // The diagnostic must cite the nullable form in its message.
    BEZEL_EXPECT_TRUE(hasMsg(r, "nullable"));
}

// =========================================================================
// §14.6 / #3271: LEGAL SHAPES — the rule admits same-key recursion and
// bare-parameter permutation cycles. The fix must not over-reject.
// =========================================================================

// LEGAL: same-key recursion. `class F<T>` with `M(): F<T>`. The
// substitution `T → T` is a bare in-scope parameter (the source's own),
// so the in-SCC edge is admitted. The recursive F<T>.M() is the
// textbook shape and must keep compiling.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           same_key_recursion_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class F<T> "
        "  public method M(): F<T> "
        "    return new F<T>(); "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§14.6"));
}

// LEGAL: bare-parameter permutation cycle. `class F<T, U>` with
// `M(): F<U, T>`. The substitution `T → U, U → T` substitutes each
// target parameter with a bare in-scope parameter (the source's own).
// Per spec: "a cycle whose edges permute or repeat bare parameters
// (F<T, U> calling F<U, T>) is equally within the rule."
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           bare_param_permutation_cycle_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class F<T, U> "
        "  public method M(): F<U, T> "
        "    return new F<U, T>(); "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§14.6"));
}

// LEGAL: a generic MEMBER's edge to its enclosing type at the enclosing
// parameters. `class F<T>` with `method M<U>(): F<U>`. The edge is from
// M's node to F's node. The substitution `T → U` substitutes F's T with
// U — U is a bare in-scope parameter at the source (M's own), so the
// edge is admitted. (F and M are NOT in the same SCC unless F's body
// also requests M at arguments, which is not the case here, so the
// per-edge rule is also vacuous.)
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_member_edge_to_enclosing_type_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class F<T> "
        "  public method M<U>(): F<U> "
        "    return new F<U>(); "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§14.6"));
}

// LEGAL: cross-SCC closed-arg substitution. `class F<T>` with
// `M(): F<int>`. The edge is from F to F (self-loop), but the
// substitution `T → int` is a closed type (no type parameter). The
// in-SCC edge substitutes the target's T with a closed type → admitted.
// The SCC = {F}; the edge is "in-SCC" per graph structure, but the
// substitution is closed, so the per-edge rule is satisfied.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           in_scc_self_loop_with_closed_arg_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class F<T> "
        "  public method M(): F<int> "
        "    return new F<int>(); "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§14.6"));
}

// =========================================================================
// §14.6 / #3271: OVER-REJECTION GUARDS — installed-base must remain clean.
// =========================================================================

// LEGAL: a generic RECORD used as the substitute's argument closed. The
// `Pair<int, int>` substitution is a closed constructed type (no source
// parameters), so it is admitted even when the source is `class F<T>`.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           in_scc_self_loop_with_closed_record_arg_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "record Pair<A, B>(a: A, b: B); "
        "class F<T> "
        "  public method M(): F<Pair<int, int>> "
        "    return new F<Pair<int, int>>(); "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§14.6"));
}

// LEGAL: a self-edge at a NULLABLE bare parameter. `class F<T>` with
// `M(): F<T?>` substitutes the target's T with `T?` — this IS rejected
// per spec text ("the nullable form T? included"), but only on an
// in-SCC edge where the substitution nests the source parameter. Here
// the source and target are the same F; the substitution is `T?` which
// is the excluded form. This IS rejected — that's the nullable
// exclusion at work. (Documented over-rejection per spec: "an in-SCC
// F<T?> edge rejects even though §7.10's nullable collapse would in
// fact bound that closure (T? is neither a bare parameter nor
// closed).")
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           in_scc_self_loop_nullable_form_rejected_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class F<T> "
        "  public method M(): F<T?> "
        "    return new F<T?>(); "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§14.6"));
}