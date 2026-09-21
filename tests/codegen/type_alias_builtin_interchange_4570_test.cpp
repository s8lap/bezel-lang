// Spec-derived regression tests for fix #4570 — §27.8 (v0.306): "Each
// instantiation of the alias is interchangeable with the corresponding
// instantiation of the aliased type"; "Type aliases — generic or not — see
// through to the aliased type for identity, equality, method dispatch, and
// overload resolution."
//
// THE BUG. A `type` alias of any ARITY-1 builtin generic collection was
// unusable: `type Al := Array<int>; var x: Al := src;` was rejected with
// "the initializer for `x` has a type that is not implicitly convertible to
// the target type (§5.1)", while the byte-identical program spelling
// `Array<int>` directly compiled and ran.
//
// ROOT CAUSE (one alias-blind head read). §27.8 alias transparency is
// implemented in `Resolver::resolveNamed` by copying the underlying TypeRef
// into the reference and then RESTORING `astNode` to the USE SITE, so
// diagnostics point at the alias mention rather than the alias declaration.
// That restore hides the underlying builtin's syntactic head, which is
// exactly why TypeRef.hpp carries `aliasResolvedHead` and documents
// `headName()` as "the ONE read every builtin-collection/wrapper identity
// check should use instead of `astNode->name->parts[0]`".
// `namedTypeSimpleName` (ExprTypecheck.cpp, plus its two file-local copies
// in ExprTypecheck_Calls.cpp / ExprTypecheck_Exprs.cpp) is such a check and
// was missing that leg, so it answered the ALIAS name. `checkAssignable`
// then reached `variantGenericAssignable` with toName="Al" against
// fromName="Array", matched no ancestor of `Array<int>` under that name,
// and — because `Array` IS in `builtinBorrowingCollectionFullyModeled` —
// took that predicate's proof-of-non-conformance REJECT.
//
// WHY ARITY-1 AND NOT ARITY-2. The asymmetry is entirely
// `builtinBorrowingCollectionFullyModeled`'s membership list, not arity as
// such. `Array`/`Set`/`Stack`/`Queue`/`LinkedList`/`SortedSet` and the
// collection interfaces are in it, so a null candidate is treated as PROOF
// of non-conformance and REJECTS. `Dictionary`/`SortedDictionary` are
// outside it (the comment there names "the arity-2 maps" explicitly), so
// they fell through to the conservative ACCEPT. The maps were therefore
// never alias-transparent either — merely un-rejected, reaching the accept
// by a residual rather than by matching. Their rows below pin that they now
// pass for the RIGHT reason and keep passing.
//
// SCOPE. `checkAssignable` serves both the local-initializer and the
// call-argument positions, so a single fix covers both; both are pinned.
// A user-declared generic (`Box<T>`) was never affected — it carries a real
// `declaration`, which `namedTypeSimpleName` reads first — and its rows are
// anti-regression only.
//
// NOT FIXED HERE (filed separately as #4583): the codegen twin.
// `isSpanLikeTypeName`
// / `isOwningSpanTypeName` (LlvmBackend_impl.hpp) read
// `astNode->name->parts[0]` directly, so a MEMBER ACCESS on an alias-typed
// `Span<T>`/`ArraySpan<T>` still fails to lower ("internal: return-value
// expression failed to lower to a value"). That is a different layer and a
// different symptom (ICE, not a false reject); this fix makes the
// assignment itself type-check, which is the §27.8 half owned here.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
// -999 on a compile-time rejection, so a pre-fix REJECT can never collide
// with a post-fix value and pass vacuously.
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ============================================================================
// §27.8 — the six arity-1 builtin heads in the modeled family. Each row is
// the issue's own repro shape at a different head: build the collection
// under its DIRECT spelling, bind it to a local annotated with the ALIAS,
// and read `Count` back through the alias-typed local. Three elements, so
// the expected value (3) is distinct from both the empty-collection answer
// (0) and the one-element answer (1) — a wrong-code regression that dropped
// elements would not read as a pass. Pre-fix every row is a compile-time
// reject (-999).
// ============================================================================

BEZEL_TEST(type_alias_builtin_interchange_4570, array_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlArr4570 := Array<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Array<int>(); "
        "    src.Add(10); src.Add(20); src.Add(30); "
        "    var x: AlArr4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

BEZEL_TEST(type_alias_builtin_interchange_4570, set_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlSet4570 := Set<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Set<int>(); "
        "    src.Add(10); src.Add(20); src.Add(30); "
        "    var x: AlSet4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

BEZEL_TEST(type_alias_builtin_interchange_4570, stack_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlStack4570 := Stack<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Stack<int>(); "
        "    src.Push(10); src.Push(20); src.Push(30); "
        "    var x: AlStack4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

BEZEL_TEST(type_alias_builtin_interchange_4570, queue_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlQueue4570 := Queue<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Queue<int>(); "
        "    src.Enqueue(10); src.Enqueue(20); src.Enqueue(30); "
        "    var x: AlQueue4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

BEZEL_TEST(type_alias_builtin_interchange_4570,
           linkedlist_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlList4570 := LinkedList<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new LinkedList<int>(); "
        "    src.AddLast(10); src.AddLast(20); src.AddLast(30); "
        "    var x: AlList4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

BEZEL_TEST(type_alias_builtin_interchange_4570,
           sortedset_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlSorted4570 := SortedSet<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new SortedSet<int>(); "
        "    src.Add(10); src.Add(20); src.Add(30); "
        "    var x: AlSorted4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

// §27.8 at an arity-1 builtin INTERFACE head. `IEnumerable` is in the same
// modeled family, so `type Al := IEnumerable<int>` took the identical
// reject — and this row additionally exercises the widening leg (the source
// is an `Array<int>`, not an `IEnumerable<int>`), which must still find the
// ancestor through the alias rather than merely matching head-to-head.
BEZEL_TEST(type_alias_builtin_interchange_4570,
           ienumerable_alias_widening_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlEnum4570 := IEnumerable<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Array<int>(); "
        "    src.Add(10); src.Add(20); src.Add(30); "
        "    var x: AlEnum4570 := src; "
        "    var n: int := 0; "
        "    for var v in x do n := n + v; end for; "
        "    return n; "
        "  end Main; "
        "end C;"), 60);
}

// ============================================================================
// §27.8 in ARGUMENT position. `checkAssignable` serves the call-argument
// path too, and the issue reported the same reject there ("the argument `a`
// to call to `Take` has a type that is not implicitly convertible …").
// ============================================================================

BEZEL_TEST(type_alias_builtin_interchange_4570, array_alias_argument_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlArg4570 := Array<int>; "
        "public class C "
        "  public static method Take(a: AlArg4570): int "
        "    return a.Count; "
        "  end Take; "
        "  public static method Main(): int "
        "    var src := new Array<int>(); "
        "    src.Add(10); src.Add(20); src.Add(30); "
        "    return Take(src); "
        "  end Main; "
        "end C;"), 3);
}

// A chained alias (`type A := B;` where `type B := Array<int>;`) must be
// equally interchangeable — §27.8 "Aliases of aliases are permitted". The
// chained case carries `aliasResolvedHead` forward from the inner alias, so
// it exercises the propagation leg rather than the first-hop assignment.
BEZEL_TEST(type_alias_builtin_interchange_4570,
           chained_array_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlInner4570 := Array<int>; "
        "type AlOuter4570 := AlInner4570; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Array<int>(); "
        "    src.Add(10); src.Add(20); src.Add(30); "
        "    var x: AlOuter4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

// ============================================================================
// The arity-2 maps. These COMPILED before the fix, but only by falling
// through `builtinBorrowingCollectionFullyModeled`'s membership test to the
// conservative accept — they were never matched through the alias. Pin that
// they still work now that the alias resolves to the real head and the
// match is genuine.
// ============================================================================

BEZEL_TEST(type_alias_builtin_interchange_4570,
           dictionary_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlDict4570 := Dictionary<int, int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Dictionary<int, int>(); "
        "    src.Add(1, 10); src.Add(2, 20); src.Add(3, 30); "
        "    var x: AlDict4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

BEZEL_TEST(type_alias_builtin_interchange_4570,
           sorteddictionary_alias_local_init_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AlSDict4570 := SortedDictionary<int, int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new SortedDictionary<int, int>(); "
        "    src.Add(1, 10); src.Add(2, 20); src.Add(3, 30); "
        "    var x: AlSDict4570 := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

// ============================================================================
// Anti-regression. The DIRECT spelling and the user-declared generic were
// both already correct; the fix must not disturb either. The user-declared
// rows matter because they take `namedTypeSimpleName`'s `declaration` leg,
// which is ordered BEFORE the new `aliasResolvedHead` leg.
// ============================================================================

BEZEL_TEST(type_alias_builtin_interchange_4570,
           direct_array_local_init_anti_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Array<int>(); "
        "    src.Add(10); src.Add(20); src.Add(30); "
        "    var x: Array<int> := src; "
        "    return x.Count; "
        "  end Main; "
        "end C;"), 3);
}

BEZEL_TEST(type_alias_builtin_interchange_4570,
           user_generic_alias_anti_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box4570<T> public v: T; end Box4570; "
        "type AlBox4570 := Box4570<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Box4570<int>(); "
        "    src.v := 42; "
        "    var x: AlBox4570 := src; "
        "    return x.v; "
        "  end Main; "
        "end C;"), 42);
}

// §5.1 must still REJECT a genuine non-conformance reached through an alias.
// The fix makes the head visible; it must not make the check permissive.
// `Stack<int>` does not conform to `IList<int>` (stdlib §4: Stack conforms
// only to IReadOnlyCollection), and the #1851 reject that
// `builtinBorrowingCollectionFullyModeled` exists to deliver must still fire
// when the target is spelled through an alias.
BEZEL_TEST(type_alias_builtin_interchange_4570,
           alias_does_not_admit_non_conforming_source_5_1) {
    auto r = runAot(
        "module M; "
        "type AlIList4570 := IList<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Stack<int>(); "
        "    src.Push(10); "
        "    var x: AlIList4570 := src; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
}

// §4.15 (#1245): a ReadOnly span is NOT assignable to its writable
// counterpart — that would allow writes through a read-only borrow. The
// check that delivers this rejection compares BOTH sides through
// `namedTypeSimpleName`, so it is the sharpest available probe that the fix
// did not make the function permissive: with the head now visible the
// rejection must still fire when the source is spelled through an alias.
BEZEL_TEST(type_alias_builtin_interchange_4570,
           readonly_span_alias_not_assignable_to_writable_4_15) {
    auto r = runAot(
        "module M; "
        "type AlRO4570 := ReadOnlyArraySpan<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(1); "
        "    var ro: AlRO4570 := a.AsReadOnlySpan(); "
        "    var w: ArraySpan<int> := ro; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
}

// §15.4's collection-literal-cannot-target-an-interface rejection was
// already firing through an alias, but via the broader fallback gate, whose
// diagnostic named the alias — spelling `type Al := IEnumerable<int>` as the
// type `Al<int>`, which is not even spellable (the alias is non-generic).
// With the head visible, the specific §15.4 interface diagnostic fires and
// names the real head. The VERDICT is unchanged (still rejected); this pins
// the message so the improvement is not silently lost.
BEZEL_TEST(type_alias_builtin_interchange_4570,
           collection_literal_interface_alias_target_names_real_head_15_4) {
    auto r = runAot(
        "module M; "
        "type AlLit4570 := IEnumerable<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var xs: AlLit4570 := [1, 2, 3]; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot target the interface type `IEnumerable`"));
}
