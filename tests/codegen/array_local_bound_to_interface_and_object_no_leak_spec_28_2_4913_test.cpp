// Spec-derived regression tests for issue #4913 (v0.307): a borrowed
// `Array<T>` local that is bound to an interface (or `Object`) view
// LEAKS the array's backing storage at scope end. Per §28.2 / §8.6
// (v0.307), converting a borrowed Array local to an Object/interface
// view must not claim ownership or leave an extra +1 — the
// interface/Object slot is a BORROW of the array's backing (§28.11:
// the backing is reclaimed with the instance; the slot's own
// scope-end disposal frees only the wrapper/value, never the
// backing), so the original local is still the sole owner at its
// own scope end.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp,
// `collectSafeViewAliases`). The safe-view-alias predicate admitted
// ONLY the three iterator-format enumerable heads (IEnumerable /
// IOwningEnumerable / IOrderedEnumerable), all of which are
// declaration-less generic specializations through the §29.2
// representation path. A `var o: Object := a;` (`Object` is a
// `PrimitiveType::Object`, not a Named) and a `var l: IList<char> :=
// a;` / `IReadOnlyList<char> := a;` / `IReadOnlyCollection<char> :=
// a;` / `ICollection<char> := a;` (all CONCRETE interface heads
// Array<T> conforms to per stdlib §4.1) fell through. The escape
// walker (`StringEscapeWalker`'s `LocalDecl` arm at ~L1740) then
// taints the array `a` on the unrecognized-view binding and excludes
// it from `borrowingArrayFreeEligible_`. With `a` not in that set,
// `lowerLocalDecl`'s scope-end `bz_array_free(backing, null)` reclaim
// never fires, and the bz_array_t header + data buffer leak
// (per-issue measurement: 120 B / 2 allocations per iteration —
// 100 iter × 120 B = 12000 B / 200 allocs in 100 iterations).
//
// THE FIX. Extend `collectSafeViewAliases` to admit the builtin
// `Object` primitive AND the six interface heads in
// `common::isBuiltinForInEnumerableHead` (IEnumerable /
// IReadOnlyCollection / IReadOnlyList / ICollection / IList /
// IOwningEnumerable — the existing `kBuiltinForInEnumerableHeads`
// predicate is the authority for "an interface view of a
// bz_array_t-backed collection is a borrow"). Distinct shape from
// the existing `kSyncIterFormatEnumerableHeads` arm (the three
// iterator-format enumerable heads are declaration-less generic
// specializations — `!dt->declaration` is part of THAT predicate's
// test); the new arm accepts heads with a real declaration AND the
// builtin `Object` primitive. The per-view escape walk
// (`collectSafeViewAliases`'s second loop) still runs on each
// admitted view to prove the view itself never escapes past `a`'s
// scope, so admitting them is sound — the walker only treats the
// binding as a borrow when the view is independently proven
// non-escaping.
//
// Spec: §28.2 / §8.6 (v0.307), §28.9 / §28.11 (Array<T> trivial-dispose;
// backing reclaimed with the instance), stdlib §4.1 (Array<T> conforms
// to IEnumerable<T> / IReadOnlyCollection<T> / IReadOnlyList<T> /
// ICollection<T> / IList<T> / Object).

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999,
// distinct from any legal exit value. runAotChecked sets
// opts.sanitizeAddress by default and runs the AOT-generated
// executable under LeakSanitizer — a leak shows up as r.ran==false.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §28.2 / §8.6 (#4913): the issue's reported repro. `var a: Array<char> :=
// ['a','b'];` followed by BOTH `var e: IEnumerable<char> := a;` AND
// `var o: Object := a;` inside a 100-iteration loop. 100 × (header +
// data buffer) = 12000 B / 200 allocs leak without the fix; with the
// fix every allocation is freed exactly once at `a`'s scope end
// (LSan-clean). The `e` binding was already a safe-view alias (in the
// pre-fix `kSyncIterFormatEnumerableHeads` set) — but `o` was NOT,
// and `a` was still tainted via `o`. The `sum` accumulator is sized so
// the answer stays well clear of runAot's 8-bit WEXITSTATUS masking
// (100 iter × 2 = 200).
BEZEL_TEST(array_local_bound_to_interface_and_object_no_leak_4913,
           issue_repro_object_and_ienumerable_spec_28_2) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..<100 do "
        "    var a: Array<char> := ['a','b']; "
        "    var e: IEnumerable<char> := a; "
        "    var o: Object := a; "
        "    sum := sum + a.Length; "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    // Pre-fix: -999 (LSan leak: 12000 B / 200 allocs). Post-fix: 200.
    BEZEL_EXPECT_EQ(runChecked(src), 200);
}

// §28.2 / §8.6 (#4913): the OBJECT-ONLY binding. This is the
// minimum-blast-radius cell that exposes the defect: a `var o: Object
// := a;` (Object is `PrimitiveType::Object`, not a Named TypeRef —
// the existing `!dt->declaration && isSyncIterFormatEnumerableHead`
// arm cannot see it because the head check fails too). 100 iter × 120
// B = 12000 B / 200 allocs leak without the fix.
BEZEL_TEST(array_local_bound_to_interface_and_object_no_leak_4913,
           object_only_binding_spec_28_2) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..<100 do "
        "    var a: Array<char> := ['a','b']; "
        "    var o: Object := a; "
        "    sum := sum + a.Length; "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    // Pre-fix: -999 (LSan leak). Post-fix: 200.
    BEZEL_EXPECT_EQ(runChecked(src), 200);
}

// §28.2 / §8.6 (#4913): the ILIST-binding twin of the OBJECT case.
// Same shape, different declared view head (`IList<char>` is a
// Named-with-declaration TypeRef, unlike Object's PrimitiveType). The
// kBuiltinForInEnumerableHeads predicate already treats IList as a
// borrowing enumerable view (the for-in desugaring admits it), but
// `collectSafeViewAliases`'s iterator-format-only predicate missed it
// because `kSyncIterFormatEnumerableHeads` does not list IList.
// Confirms the new arm's `isBuiltinForInEnumerableHead` widening
// fires for IList (per the stdlib §4.1 conformance list).
BEZEL_TEST(array_local_bound_to_interface_and_object_no_leak_4913,
           ilist_binding_spec_28_2) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..<100 do "
        "    var a: Array<char> := ['a','b']; "
        "    var l: IList<char> := a; "
        "    sum := sum + a.Length; "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    // Pre-fix: -999 (LSan leak: 11880 B / 198 allocs). Post-fix: 200.
    BEZEL_EXPECT_EQ(runChecked(src), 200);
}

// §28.2 / §8.6 (#4913): the IREADONLYLIST-binding twin. Same shape,
// different head — confirms the widening is head-name-driven (the
// `isBuiltinForInEnumerableHead` predicate is the authority).
BEZEL_TEST(array_local_bound_to_interface_and_object_no_leak_4913,
           ireadonlylist_binding_spec_28_2) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..<100 do "
        "    var a: Array<char> := ['a','b']; "
        "    var r: IReadOnlyList<char> := a; "
        "    sum := sum + a.Length; "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    // Pre-fix: -999 (LSan leak: 11880 B / 198 allocs). Post-fix: 200.
    BEZEL_EXPECT_EQ(runChecked(src), 200);
}

// §28.2 / §8.6 (#4913): Array<int> bound to Object. Same
// backing-only reclaim path — the per-element policy differs (char
// vs int), but the slot's scope-end disposal is the
// `bz_array_free(backing, null)` reclaim (no per-element disposer for
// value types). Confirms the fix isn't accidentally gated on the
// element type.
BEZEL_TEST(array_local_bound_to_interface_and_object_no_leak_4913,
           array_int_to_object_spec_28_2) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..<100 do "
        "    var a: Array<int> := [1, 2]; "
        "    var o: Object := a; "
        "    sum := sum + a.Length; "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    // Pre-fix: -999 (LSan leak: 12000 B / 200 allocs). Post-fix: 200.
    BEZEL_EXPECT_EQ(runChecked(src), 200);
}

// §28.2 / §8.6 (#4913) CONTROL — `var a: Array<char> := ['a','b'];`
// with NO interface/Object binding. The array's local slot's
// scope-end `bz_array_free` reclaim (the pre-existing
// `borrowingArrayFreeEligible_` path, lit #1871 / #1003) already
// handled this case — no fix is needed here. The cell proves the
// fix's widening did not pull an UNRELATED case into a regression
// (in particular: no spurious Object-string-retain logic should now
// fire on a non-Object binding).
BEZEL_TEST(array_local_bound_to_interface_and_object_no_leak_4913,
           control_no_view_binding_spec_28_2) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..<100 do "
        "    var a: Array<char> := ['a','b']; "
        "    sum := sum + a.Length; "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    // Pre-fix AND post-fix: 200 (LSan-clean — the array's local slot
    // reclaim handles it without any view binding).
    BEZEL_EXPECT_EQ(runChecked(src), 200);
}

// §28.2 / §8.6 (#4913) CONTROL — `var f: Foo := new Foo(); var o:
// Object := f;`. A regular class instance bound to Object uses
// `registerUsingDisposal` (the using-var / class-disposal path) OR
// the `armStringLocalRefcount` Object-string-aliased path. Neither
// is in scope for this fix. Pre-fix AND post-fix: 100 (LSan-clean
// — a regular class instance, not a borrowing-Array local, takes
// the class disposal route).
BEZEL_TEST(array_local_bound_to_interface_and_object_no_leak_4913,
           control_regular_class_to_object_spec_28_2) {
    std::string src =
        "module M; "
        "class Foo end Foo; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..<100 do "
        "    var f: Foo := new Foo(); "
        "    var o: Object := f; "
        "    sum := sum + 1; "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    // Pre-fix AND post-fix: 100 (the class disposal path is
    // unrelated to this fix).
    BEZEL_EXPECT_EQ(runChecked(src), 100);
}
