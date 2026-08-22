// fix #3342 (§26.5 / stdlib §4.4, v0.278): instance members block same-named
// extensions BY NAME — of any arity, applicability, or accessibility. The
// stdlib §4.4 worked example is `arr.Count(pred)`: `Array<T>` declares `Count`
// as a 0-arg property, so the call's arity (1) does not match the instance
// member's arity (0), instance resolution commits by name, the property has
// no applicable overload for the 1-arg call, and per §26.5 rule 1 "the call
// is a compile error citing the instance member(s); it does not fall back to
// an extension". The pre-#3342 path let the §5 LINQ `Count(pred)` resolve
// because the receiver's TypeSymbol::members list is empty for builtins (the
// surface is hardcoded across sema), so `lookupMember` returned null and the
// extension lookup ran unchallenged. These tests pin the rejection (and the
// message that cites the instance member) for the six stdlib-listed names
// (`Count`, `Contains`, `First`, `Last`, `Reverse`, `ToArray`) across the
// receiver types whose declared surface includes each one.
//
// Axis grid (per the lane SOP):
//   * receiver type: Array<T> | OwningArray<T> | List<T> | Set<T> |
//                    Dictionary<K,V> | LinkedList<T> | SortedSet<T> |
//                    FrozenArray<T> | string (no blocking)
//   * member name:   Count | Contains | First | Last | Reverse | ToArray
//   * arity axis:    instance-member arity (Count=0, Contains=1, …) vs the
//                    call's arity (0 / 1 / 2) — match = ACCEPTED, mismatch
//                    = REJECTED
//   * operator source: §5 LINQ extension (the one the bug let through) vs
//                    the same-named operator spelled on a non-Array receiver
//                    (still ACCEPTED — no blocking on the non-Array type)

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

// Look for an error whose message cites the instance member and names the
// extension-blocking rule (§26.5 rule 1). Pinning the wording — not just
// `hasErrors()` — is the regression-test discipline: a branch-order
// change that routes the call to the wrong reject (the catch-all "no
// member" reject, say) would still flip `hasErrors()` but would miss the
// specific wording the rule requires.
bool hasInstanceBlocksExtensionError(const bezel::test::SemaResult& r,
                                     std::string_view memberName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("instance member") == std::string::npos) continue;
        if (d.message.find(std::string(memberName)) == std::string::npos) continue;
        if (d.message.find("26.5") == std::string::npos) continue;
        if (d.message.find("rule 1") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// ============================================================================
// #3342 worked example (stdlib §4.4): `arr.Count(pred)` on Array<T>.
// Array<T> declares `Count` as a 0-arg property; the call's arity is 1.
// Per §26.5 rule 1, the call is a compile error citing the instance member.
// ============================================================================
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           array_count_pred_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); xs.Add(3); "
        "  return xs.Count(lambda(n: int): bool => n > 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstanceBlocksExtensionError(r, "Count"));
}

// `arr.Count(pred)` over records (the test the stdlib cites from the
// existing corpus, `tests/codegen/linq_test.cpp:402`). Same rule.
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           array_count_pred_record_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(5, 0)); a.Add(new Point(3, 9)); a.Add(new Point(5, 7)); "
        "  return a.Count(lambda(p) => p.x = 5); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstanceBlocksExtensionError(r, "Count"));
}

// ============================================================================
// Control axis (must still work): `arr.Count()` (0 args) on Array<T> matches
// the instance member's arity (0), so it is NOT rejected — the §5 LINQ
// `Count()` extension happens to share the property's name and is therefore
// unreachable, but the call's arity matches the instance member and the
// resolution succeeds. The stdlib §4.4 wording allows this: the COMPOSED
// spelling `coll.Where(pred).Count()` is the documented route to the §5
// operator pipeline when the receiver declares `Count`.
// ============================================================================
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           array_count_zero_args_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); xs.Add(3); "
        "  return xs.Count(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same control axis on the §5 LINQ operator pipeline: `arr.Where(pred).Count()`
// — the documented composed spelling — still works (the Where receiver is
// `IEnumerable<int>`, not `Array<int>`, so no instance `Count` blocks it).
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           array_where_pred_count_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  return a.Where(lambda(x) => x mod 2 = 0).Count(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// OwningArray<T> axis: §4.5 declares `Count` as a 0-arg property on
// OwningArray<T> too — the same blocking applies.
// ============================================================================
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           owning_array_count_pred_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var xs := new OwningArray<int>(); "
        "  xs.Add(1); xs.Add(2); "
        "  return xs.Count(lambda(n: int): bool => n > 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstanceBlocksExtensionError(r, "Count"));
}

// ============================================================================
// LinkedList<T> axis: §4.6 declares `First`/`Last` as `T?` properties, so
// the §5 LINQ `First(pred)` / `Last(pred)` extensions on a LinkedList<T>
// receiver are unreachable. The 0-arg `First()` form (instance property,
// no args) is the legitimate spelling and must still be accepted — the §5
// extension `First()` happens to share the name but is also reachable on
// other receivers where `First` isn't a property.
// ============================================================================
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           linkedlist_first_pred_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var xs := new LinkedList<int>(); "
        "  xs.AddLast(1); xs.AddLast(2); "
        "  return xs.First(lambda(n: int): bool => n > 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstanceBlocksExtensionError(r, "First"));
}

BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           linkedlist_last_pred_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var xs := new LinkedList<int>(); "
        "  xs.AddLast(1); xs.AddLast(2); "
        "  return xs.Last(lambda(n: int): bool => n > 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstanceBlocksExtensionError(r, "Last"));
}

// ============================================================================
// SortedSet<T> axis: §4.18 declares `First` / `Last` as 0-arg properties
// (the least/greatest element; throws InvalidOperationException if
// empty). A same-named §5 LINQ operator (First(pred), Last(pred) on
// IEnumerable<T>) is therefore unreachable by name on a SortedSet<T>
// receiver (§26.5 rule 1). The 1-arg `First(pred)` shape is the
// worked-example mirror of the LinkedList case above.
// ============================================================================
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           sortedset_first_pred_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<int>(); "
        "  ss.Add(1); ss.Add(2); ss.Add(3); "
        "  return ss.First(lambda(n: int): bool => n > 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstanceBlocksExtensionError(r, "First"));
}

BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           sortedset_last_pred_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<int>(); "
        "  ss.Add(1); ss.Add(2); ss.Add(3); "
        "  return ss.Last(lambda(n: int): bool => n > 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstanceBlocksExtensionError(r, "Last"));
}

// Control axis (must still work): the 0-arg `ss.First` form matches
// the §4.18 instance member's arity and resolves cleanly — the §5
// extension is unreachable by name on a SortedSet<T> receiver, but
// the property's reference form is exactly what §26.5 step 1 commits
// to. (No-op for typed-typed-result access — the property carries
// value semantics so a bare read returns a copy, no borrow-return
// check trips.)
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           sortedset_first_zero_args_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<int>(); "
        "  ss.Add(5); ss.Add(2); ss.Add(8); "
        "  return ss.First; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// FrozenArray<T> axis: §4.19 declares `ToArray` (returns owned Array<T>,
// a fresh mutable copy), so the §5 LINQ `ToArray()` extension is
// unreachable on a FrozenArray<T> receiver. The 1-arg `ToArray(pred)`
// shape is the worked-example mirror of the Count case: a receiver
// declaring a 0-arg instance member cannot route the predicate form to
// the same-named §5 LINQ operator (rule 1's "same-named" gate is by
// name — arity mismatch falls to the "instance lookup commits, then
// fails — compile error" case).
// ============================================================================
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           frozen_array_to_array_pred_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var fa := FrozenArray<int>.From(new Array<int>()); "
        "  return fa.ToArray(lambda(x: int) => x > 0).Count(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstanceBlocksExtensionError(r, "ToArray"));
}

// Same axis, control: the 0-arg form matches the instance member's
// arity (the spec's §26.5 rule 1 application — instance resolution
// commits, the property handles the call, the §5 extension is just
// unreachable by name on this receiver). `FrozenArray<T>.Length`
// reads the §4.19 length property on the source so we don't have to
// chain through `ToArray()` (which would test something orthogonal).
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           frozen_array_length_zero_args_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var fa := FrozenArray<int>.From(new Array<int>()); "
        "  return fa.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// Negative axis: `arr.First(pred)` on an Array<T> is NOT blocked —
// Array<T> does NOT declare `First` (no instance member of that name),
// so the §5 LINQ `First(pred)` extension resolves cleanly. This pins
// the scope of the blocking table: ONLY types whose declared surface
// includes the name are gated, not every collection type.
// ============================================================================
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           array_first_pred_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return a.First(lambda(x: int) => x = 2); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same negative axis for `ToArray`: Array<T> DOES declare `ToArray()`
// (§4.5), but the 0-arg call matches the 0-arg instance member and
// resolves cleanly — the §5 extension is unreachable, the instance
// member handles the call. Array<T> has `Count`, not `Length` (stdlib
// §1's Count-vs-Length rule); the chained `.Count` reads the §4.5
// property on the freshly-copied Array<T>.
BEZEL_TEST(sema_builtin_collection_blocks_extension_3342,
           array_to_array_zero_args_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  using var b := a.ToArray(); "
        "  return b.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
