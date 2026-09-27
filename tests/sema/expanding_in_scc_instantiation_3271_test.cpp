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
// =========================================================================
// §14.6 / #3271: the generic-MEMBER route.
//
// §14.6's graph makes "each generic type … a node, and each generic member
// … its own node", and the consequences paragraph names the call route in
// exactly those terms: "a call `F<Pair<T, T>>` inside `F<T>`'s body". Every
// test above exercises that shape where the node is a generic TYPE.
//
// A generic METHOD's self-request is a different node class, and the
// type-node graph cannot see it: resolving `Depth<Pair<T, T>>(…)` inside
// `Depth<T>` records only the ARGUMENT `Pair<T, T>` as an instantiation of
// `Pair` — an edge from `Util` to `Pair` that closes no cycle. The
// `Depth` → `Depth` edge, which is the self-loop §14.6 rejects, had no
// representation at all.
//
// Pre-fix state, measured on real `bezelc` (not the sema harness): the two
// spellings below compile, link, and RUN. The monomorphizer collapses the
// unbounded specialization family `Depth<int>`, `Depth<Pair<int, int>>`,
// `Depth<Pair<Pair<…>>>`, … onto three mangled keys
// (`Util$Depth$spec`, `Util$Depth$spec.1`, `Util$Depth$spec$Pair`), so the
// shape does not even announce itself as a hang — it silently reuses one
// specialization for a family of distinct types.
//
// Scope of the rejection added here (the residual is recorded in the
// implementation comment and as a follow-up, not guessed at): only the
// DIRECT self-loop, spelled with EXPLICIT type arguments, is decided.
// A self-loop is the one member-edge class whose SCC membership needs no
// resolved call graph — an edge from a node to itself is unconditionally
// inside its own component. Mutual member cycles and inference-supplied
// type arguments both need the call graph and stay accepted; both
// under-approximate, which is the safe direction for this rule.
// =========================================================================

// REJECTED — the generic-method call route, unqualified self-call.
// `Depth<T>` requests `Depth<Pair<T, T>>`: the substitution at the
// member's own parameter position is neither a bare in-scope parameter
// (it is `Pair<T, T>`) nor a closed type (it contains T).
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_self_call_at_constructed_arg_rejected_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "static class Util "
        "  public static method Depth<T>(x: T, n: int): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return Depth<Pair<T, T>>(p, n - 1); "
        "  end Depth; "
        "end Util;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3271"));
    // The diagnostic names the cycle (the member) and the growing argument.
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Depth`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "Pair<T, T>"));
}

// REJECTED — the same edge spelled `this.M<…>` on an instance method.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_this_qualified_self_call_rejected_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Runner "
        "  public method Go<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return this.Go<Pair<T, T>>(p); "
        "  end Go; "
        "end Runner;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Go`"));
}

// REJECTED — the same edge spelled `C.M<…>` (enclosing-type-qualified).
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_type_qualified_self_call_rejected_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "static class Util "
        "  public static method Go<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return Util.Go<Pair<T, T>>(p); "
        "  end Go; "
        "end Util;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Go`"));
}

// REJECTED — the same edge spelled `this?.M<…>` (issue #5027 follow-up:
// `Resolver::recordGenericMemberSelfEdge` handled IdentifierExpr and
// MemberAccessExpr callees but not NullCondMemberExpr, so a `?.`-spelled
// self-call went unrecorded). `this` is never null (§7.9), so `this?.Go<…>`
// is the identical access as `this.Go<…>` wrapped in a no-op null-conditional
// — the same expanding instantiation, and MUST be rejected on the same
// terms. Pre-fix this compiled past sema (no §14.6 diagnostic at all) and,
// carried into codegen, hung the compiler in monomorphization instead of
// being rejected at the declaration.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_nullcond_this_qualified_self_call_rejected_spec_14_6_issue_5027) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Runner "
        "  public method Go<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return this?.Go<Pair<T, T>>(p) ?? 0; "
        "  end Go; "
        "end Runner;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Go`"));
}

// LEGAL — the `this?.M<…>` spelling of ordinary uniform self-recursion (the
// bare-parameter admitted shape, §5027's over-rejection control): the fix
// must record the edge and let `substitutionIsAdmitted` clear it, not reject
// every `?.`-spelled self-call outright.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_nullcond_this_qualified_bare_param_admitted_spec_14_6_issue_5027) {
    auto r = parseSemaAll(
        "module M; "
        "class Runner "
        "  public method Depth<T>(x: T, n: int): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + (this?.Depth<T>(x, n - 1) ?? 0); "
        "  end Depth; "
        "end Runner;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// REJECTED — the nullable form on a member self-edge. Spec: "an argument
// containing an in-scope type parameter other than as the bare argument
// itself, the nullable form `T?` included".
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_self_call_nullable_form_rejected_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "static class Util "
        "  public static method Go<T>(x: T): int "
        "    return Go<T?>(null); "
        "  end Go; "
        "end Util;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "nullable"));
}

// =========================================================================
// §14.6 / #3271: OVER-REJECTION GUARDS for the generic-member route. The
// failure mode this rule must not have is rejecting legitimate generic
// recursion, so each admitted shape §14.6 names gets its own control.
// =========================================================================

// LEGAL — uniform recursion at the same type argument. The textbook shape:
// `Depth<T>` calling `Depth<T>` substitutes the source's own bare parameter.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_self_call_bare_param_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "static class Util "
        "  public static method Depth<T>(x: T, n: int): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + Depth<T>(x, n - 1); "
        "  end Depth; "
        "end Util;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// LEGAL — a permutation cycle over bare parameters. Spec: "a cycle whose
// edges permute or repeat bare parameters (`F<T, U>` calling `F<U, T>`) is
// equally within the rule."
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_self_call_permutation_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "static class Util "
        "  public static method Two<T, U>(a: T, b: U, n: int): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + Two<U, T>(b, a, n - 1); "
        "  end Two; "
        "end Util;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// LEGAL — a closed type argument. `Depth<T>` calling `Depth<int>` bounds
// the closure by construction; the rule's second admitted arm.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_self_call_closed_arg_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "static class Util "
        "  public static method Depth<T>(x: T, n: int): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + Depth<int>(7, n - 1); "
        "  end Depth; "
        "end Util;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// LEGAL — a bare parameter of the ENCLOSING declaration. Spec: the bare
// parameter may be "the source's own, or an enclosing declaration's", so a
// member self-call at the enclosing type's `X` is admitted.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_self_call_enclosing_type_param_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder<X> "
        "  public value: X; "
        "  public constructor(v: X) "
        "    this.value := v; "
        "  end constructor; "
        "  public method Walk<T>(t: T, n: int): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + this.Walk<X>(this.value, n - 1); "
        "  end Walk; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// LEGAL — a growing argument on an edge to a DIFFERENT member. `Wrap<T>`
// requesting `Consume<Pair<T, T>>` is not a self-loop, and `Consume` does
// not request `Wrap` back, so the two members are in different components
// and §14.6's per-edge condition does not apply to the edge at all. This is
// the control that a name-blind rejection would fail.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_edge_to_other_member_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "static class Util "
        "  public static method Wrap<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return Consume<Pair<T, T>>(p); "
        "  end Wrap; "
        "  public static method Consume<U>(u: U): int "
        "    return 1; "
        "  end Consume; "
        "end Util;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// LEGAL (conservatively) — an OVERLOADED member name. The recorder matches
// the callee by name; with two members named `Go` the call may bind to the
// other overload, which §14.6 makes a different node. Attributing the edge
// on a name match alone would be exactly the over-rejection this rule must
// not commit, so the overload guard declines to decide.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_overloaded_name_not_attributed_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "static class Util "
        "  public static method Go<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return Go<Pair<T, T>>(p, 1); "
        "  end Go; "
        "  public static method Go<U>(u: U, k: int): int "
        "    return k; "
        "  end Go; "
        "end Util;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// LEGAL — a same-named member on a DIFFERENT receiver. `h.Go<Pair<T, T>>`
// names `Helper.Go`, not the enclosing `Caller.Go`; the receiver guard
// keeps the edge unattributed.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_other_receiver_same_name_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Helper "
        "  public method Go<U>(u: U): int "
        "    return 1; "
        "  end Go; "
        "end Helper; "
        "class Caller "
        "  public method Go<T>(x: T, h: Helper): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return h.Go<Pair<T, T>>(p); "
        "  end Go; "
        "end Caller;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// LEGAL — a NON-generic method that calls itself. §14.6 attaches a
// non-generic member's edges to its declaring type's node; the member has no
// parameters to substitute, so there is nothing for this rule to decide.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           non_generic_method_self_call_admitted_spec_14_6_issue_3271) {
    auto r = parseSemaAll(
        "module M; "
        "static class Util "
        "  public static method Count(n: int): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + Count(n - 1); "
        "  end Count; "
        "end Util;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// =========================================================================
// §14.6 / #5045: variable-receiver self-calls.
// fix #5045 (§14.6, v0.307): a generic method calling itself through a
// VARIABLE receiver of the enclosing type — `other.Go<Pair<T,T>>(p, other)`
// inside `Go<T>` on class `Runner` — was not recorded as a self-edge because
// `recordGenericMemberSelfEdge` only accepted `this`, the type name, or
// `IdentifierExpr` with no receiver at all. A local variable or method
// parameter typed as the enclosing type is a compile-time alias for `this`
// and produces the identical access; the expanding instantiation check must
// run on it too. Both the dot form (`other.Go<...>`) and the null-cond
// form (`other?.Go<...>() ?? 0`) are covered.
//
// The NullCondCallExpr arm (`other?.Go<...>() ?? 0`) parses as a BinaryExpr
// with op=="??" and left==NullCondCallExpr; the BinaryExpr arm previously
// recursed but did not call `recordGenericMemberSelfEdge`, so that shape
// had no edge recording at all.
// =========================================================================

// REJECTED — dot form, variable receiver of enclosing type.
// `Runner.Go<T>` calling `other.Go<Pair<T, T>>` where `other: Runner`:
// the substitution `T → Pair<T, T>` is neither bare (it's `Pair<T, T>`)
// nor closed (it contains T). The expanding instantiation must be rejected.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_variable_receiver_dot_form_expanding_rejected_spec_14_6_issue_5045) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Runner "
        "  public method Go<T>(x: T, other: Runner): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return other.Go<Pair<T, T>>(p, other); "
        "  end Go; "
        "end Runner;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Go`"));
}

// REJECTED — null-conditional form, variable receiver.
// `other?.Go<Pair<T, T>>(p) ?? 0` inside `Go<T>` on class `Runner`:
// the `??` creates a BinaryExpr whose left is a NullCondCallExpr; the
// BinaryExpr arm did not record the edge, so the expanding instantiation
// check never ran and the compiler hung in monomorphization.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_variable_receiver_nullcond_form_expanding_rejected_spec_14_6_issue_5045) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Runner "
        "  public method Go<T>(x: T, other: Runner): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return other?.Go<Pair<T, T>>(p) ?? 0; "
        "  end Go; "
        "end Runner;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Go`"));
}

// LEGAL — uniform self-recursion through a variable receiver of the enclosing
// type. `Runner.Go<T>` calling `other.Go<T>(x, other)` where `other: Runner`:
// the substitution `T → T` is bare (the source's own parameter), so the
// in-SCC self-loop is admitted.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_variable_receiver_uniform_self_call_admitted_spec_14_6_issue_5045) {
    auto r = parseSemaAll(
        "module M; "
        "class Runner "
        "  public method Depth<T>(x: T, n: int, other: Runner): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + other.Depth<T>(x, n - 1, other); "
        "  end Depth; "
        "end Runner;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// §14.6 / #5082: variable-receiver self-edge — locals and fields.
// The #5045 fix added recordGenericMemberSelfEdge overloads for
// MemberAccessExpr and NullCondMemberExpr, but only accepted `this` and
// `C.this` spellings. fix #5082 (§14.6, v0.307) extends both arms to
// also accept a local variable or field whose declared type is the enclosing
// type — the same substitution that `this` carries — so `local.Go<…>(…)`
// and `this.field.Go<…>(…)` record a self-edge and the expanding-
// instantiation check fires on them too.
// =========================================================================

// ADMITTED — field receiver with bare substitution.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_field_receiver_bare_subst_admitted_spec_14_6_issue_5082) {
    auto r = parseSemaAll(
        "module M; "
        "class Runner "
        "  public other: Runner; "
        "  public constructor() "
        "    this.other := new Runner(); "
        "  end constructor; "
        "  public method Depth<T>(x: T, n: int): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + this.other.Depth<T>(x, n - 1); "
        "  end Depth; "
        "end Runner;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// ADMITTED — NullCondMemberExpr with bare `this?.` (existing behavior).
// This controls that the NullCondMemberExpr arm doesn't over-reject.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_nullcond_this_bare_subst_admitted_spec_14_6_issue_5082) {
    auto r = parseSemaAll(
        "module M; "
        "class Runner "
        "  public other: Runner?; "
        "  public constructor() "
        "    this.other := new Runner(); "
        "  end constructor; "
        "  public method Depth<T>(x: T, n: int): int "
        "    if n <= 0 then "
        "      return 0; "
        "    end if; "
        "    return 1 + (this?.Depth<T>(x, n - 1) ?? 0); "
        "  end Depth; "
        "end Runner;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}

// REJECTED — NullCondMemberExpr with expanding substitution via `this?.`.
// The `?.` on `this` is a no-op wrapper; the expanding substitution is the
// same as the plain `this.Go<Pair<T,T>>` case and is correctly rejected.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_nullcond_this_expanding_subst_rejected_spec_14_6_issue_5082) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Runner "
        "  public method Go<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return (this?.Go<Pair<T, T>>(p) ?? 0); "
        "  end Go; "
        "end Runner;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Go`"));
}


// REJECTED — field receiver with expanding substitution via dot form.
// fix #5082 (§14.6, v0.307): `this.other.Go<Pair<T,T>>(p)` inside `Go<T>`
// is a MemberAccessExpr whose target is another MemberAccessExpr
// (`this.other`). The fix walks this chain to its root (`this`) and records
// the self-edge. Without the fix, no self-edge is recorded and the
// expanding substitution (Pair<T,T> substituted for T) is NOT rejected —
// the call is incorrectly admitted.  This is the actual bug case from #5082.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_field_receiver_expanding_subst_rejected_spec_14_6_issue_5082) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Runner "
        "  public other: Runner; "
        "  public constructor() "
        "    this.other := new Runner(); "
        "  end constructor; "
        "  public method Go<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return this.other.Go<Pair<T, T>>(p); "
        "  end Go; "
        "end Runner;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Go`"));
}

// NOTE on the issue title's "locals" half (#5082/#5111 relationship):
// a genuine LOCAL-variable receiver (`using var other := new Runner();` /
// `const other: Runner := ...;`, looked up via `lookupName` inside
// resolveGenericSelfEdgeReceiverType) is deliberately NOT covered by this fix
// and has no test here asserting rejection — MEASURED, not assumed: every
// `LocalSymbol` in this compiler is constructed in ExprTypecheck_Exprs.cpp or
// ExprTypecheck.cpp (`ctx_.makeSymbol<LocalSymbol>()`), both later passes;
// `Resolver::resolveTypesInStmt` (this file) walks a LocalDecl/LocalConstDecl
// only to resolve its type annotation and initializer — it registers no
// Symbol into `scope` at all. So `lookupName(rootId->name, scope)` can never
// return a LocalSymbol at the point recordGenericMemberSelfEdge runs, with or
// without an explicit `: Type` annotation on the local. An annotated-local
// variant of the test below was tried and FAILS (bezelc admits the expanding
// substitution with no §14.6 diagnostic) for exactly this reason — the same
// root cause #5111 diagnosed for its own (unannotated) repro. This lane does
// not fix #5111; see the handback report for the full comparison against
// what #5045 already covers (parameters, via a decl-level scan that does not
// need `lookupName`) and what this fix adds (fields, via a field-chain walk
// that also does not need `lookupName`).

// REJECTED — field receiver via the null-conditional form.
// fix #5082 (§14.6, v0.307): the standalone NullCondMemberExpr overload
// previously found only `this` and a params-scan variable — a field receiver
// through `?.` (`this.other?.Go<...>`, a MemberAccessExpr target rather than
// a bare identifier) was rejected outright. `other: Runner?` here is a
// FIELD, and its declared type (the enclosing type, nullable) makes this the
// same self-loop as the dot-form field-receiver case, so the expanding
// substitution must be rejected exactly as it is for the dot form.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_field_receiver_nullcond_form_expanding_rejected_spec_14_6_issue_5082) {
    auto r = parseSemaAll(
        "module M; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Runner "
        "  public other: Runner?; "
        "  public constructor() "
        "    this.other := new Runner(); "
        "  end constructor; "
        "  public method Go<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return this.other?.Go<Pair<T, T>>(p) ?? 0; "
        "  end Go; "
        "end Runner;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""14.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "generic method `Go`"));
}

// ADMITTED — field-chain receiver through an UNRELATED type must NOT be
// mistaken for a self-edge. R22 integrator review point (2) of the original
// #5082 attempt (268a6c4e, p22-5082-c27): a field-chain walk that accepts
// ANY `this.a.b...` chain rooted at `this`, without checking each link's OWN
// declared type, would misrecord `this.helper.Go<...>` as a self-edge of
// `Runner.Go` merely because `Helper` happens to also declare a same-named,
// same-arity generic method `Go<T>`. `helper: Helper` is a field of a
// DIFFERENT class, so this call is Helper.Go's own (non-recursive)
// instantiation, not a self-loop of Runner.Go, and must be admitted.
BEZEL_TEST(expanding_in_scc_instantiation_3271,
           generic_method_field_receiver_unrelated_type_admitted_spec_14_6_issue_5082) {
    auto r = parseSemaAll(
        "module M; "
        "class Helper "
        "  public method Go<T>(x: T): int "
        "    return 0; "
        "  end Go; "
        "end Helper; "
        "class Pair<A, B> "
        "  public v1: A; "
        "  public v2: B; "
        "  public constructor(a: A, b: B) "
        "    this.v1 := a; this.v2 := b; "
        "  end constructor; "
        "end Pair; "
        "class Runner "
        "  public helper: Helper; "
        "  public constructor() "
        "    this.helper := new Helper(); "
        "  end constructor; "
        "  public method Go<T>(x: T): int "
        "    using var p := new Pair<T, T>(x, x); "
        "    return this.helper.Go<Pair<T, T>>(p); "
        "  end Go; "
        "end Runner;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "\xC2\xA7""14.6"));
}
