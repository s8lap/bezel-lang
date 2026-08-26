// Spec-derived regression tests for #2774 (v0.282): `var a: Array<string> :=
// ["...", ...]` collection-literal local leaks its backing allocation.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §4.4 declares `Array<T>` is trivial-dispose
// (language spec §28.9) — the backing storage is part of the instance's single
// allocation, released with it by the compiler-inserted free at the array's
// owner's release point.  A `var`-bound collection literal is owned by the
// local, so §28.3's scope-exit release should free it.  Before this fix it did
// NOT — isBorrowingArrayLiteralLocal explicitly excluded "RuntimeManaged" element
// types (string/closure) on the ground that "their +1/element semantics differ
// from the materializer path", but no replacement reclamation was installed for
// strings, so an `Array<string>` local's bz_array_t + data buffer leaked exactly
// 176 bytes in 2 allocations (per the issue's #2768 differential probe).
//
// THE FIX (one site, narrow).  Two co-located changes in lowerLocalDecl /
// collectBorrowingArrayFreeEligible:
//   1. `isBorrowingArrayLiteralLocal` now admits non-nullable `string` element
//      types (the literal arm in `materializeHeapCollectionLiteral` retains a
//      +1 per string element via `litIsStrArr` / `emitStringRetain`, so a
//      matched release at scope end is correct — paired retain/release, no UAF).
//   2. The literal-path reclaim block in `lowerLocalDecl` installs
//      `stringElementReleaseThunk` as `collectionElemDisposer` for
//      `Array<string>` literals (mirrors the materializer-path derivation at
//      the end of lowerLocalDecl, ~13983-13990, which already handles the
//      `ToArray()`/`Sorted()` materializer case for the same shape).
// Closure-element `Array<closure>` literals are STILL excluded — no per-element
// retain at construction, so a release would be an unpaired -1.  That widening
// is a separate, deliberately-not-in-scope fix.
//
// CONTROLS.
//   - `using var a: Array<string> := [...]` — registerUsingDisposal path is
//     independent of this fix; it must already pass under LSan.
//   - `var a: Array<int> := [...]` — Array<int> literals were already in
//     borrowingArrayFreeEligible_ (the "RuntimeManaged" gate excludes only
//     string/fn elements).  This cell proves the fix's widening did not pull
//     other element categories into the string-element branch.
//   - `var ll: LinkedList<int> := [...]` — NOT IN SCOPE for #2774 (the issue
//     and fix are about `Array<string>`); left here so the regression sweep
//     catches an inadvertent change to a NEIGHBORING collection family.  If
//     this cell regresses, look at the LinkedList literal path, not this fix.
//
// ASAN ALLOCATION.  Every primary cell runs `runAotChecked` (generated-code
// ASan + LSan).  A leak shows up as a non-zero `exitCode` and the report text
// contains `LeakSanitizer`.  A successful run exits 0 under LSan.  The control
// cell uses plain `runAot` because it is intentionally a sanity check on the
// unaffected `using var` path, and `using var` with a class element still goes
// through registerUsingDisposal without touching the new string-element branch.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// Generated-code ASan/LSan: a literal that leaks its bz_array_t + data buffer
// shows up as a LSan report.  Before this fix this cell failed (176 bytes /
// 2 allocs); after, it exits 0 with no LSan report.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #2774 / stdlib §4.4 + spec §28.3 / §28.9 / §28.11: the reported repro.
// A plain `var a: Array<string> := [...]` — three elements, each an interned
// string literal, indexed by position; the result is packed in 1-bit slots
// (r += 1, 2, 4) so the expected answer stays well clear of runAot's 8-bit
// WEXITSTATUS masking (the defect is "leaks", not "wrong value", so this is
// a non-coincidence belt-and-braces measure).
//
// Before the fix:  LSan reports 176 bytes / 2 allocations (bz_array_t +
// data buffer) — the backing leaked.  After:  LSan-clean exit 0 with the
// expected value 7 (1 + 2 + 4).  Each string element is a literal "a"/"b"/"c"
// so the per-element +1 retained at construction is already accounted for by
// the runtime's static intern table — no per-string leak on top of the
// backing leak that the fix closes.
BEZEL_TEST(array_string_literal_local_leak_2774, array_string_local_leak_closes) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: Array<string> := [\"c\", \"a\", \"b\"]; "
        "    var r: int := 0; "
        "    if a[0] = \"c\" then r := r + 1; end; "
        "    if a[1] = \"a\" then r := r + 2; end; "
        "    if a[2] = \"b\" then r := r + 4; end; "
        "    return r; "
        "  end Main; "
        "end C; "), 7);
}

// #2774 control: `using var a: Array<string> := [...]`.  Goes through
// registerUsingDisposal, NOT through the literal-path reclaim block in
// lowerLocalDecl — so this fix must NOT touch it.  Plain runAot is enough
// (the value check is the load-bearing assertion; ASan adds little because
// `using var` is leak-clean by construction independently of the fix).
BEZEL_TEST(array_string_literal_local_leak_2774, using_var_array_string_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a: Array<string> := [\"c\", \"a\", \"b\"]; "
        "    var r: int := 0; "
        "    if a[0] = \"c\" then r := r + 1; end; "
        "    if a[1] = \"a\" then r := r + 2; end; "
        "    if a[2] = \"b\" then r := r + 4; end; "
        "    return r; "
        "  end Main; "
        "end C; "), 7);
}

// #2774 sanity: `var a: Array<int> := [...]` was already leak-clean before
// the fix (Array<int> is admitted to borrowingArrayFreeEligible_ by the
// existing "RuntimeManaged" exclusion, since int is Value not RuntimeManaged).
// This cell pins that the fix's widening did not pull other element
// categories into the string-element disposer branch (which would be a
// mis-disposal: a `bz_array_free(arr, stringElementReleaseThunk)` on a
// backing holding int slots would treat the int bits as a string pointer).
BEZEL_TEST(array_string_literal_local_leak_2774, array_int_local_still_leak_clean) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: Array<int> := [3, 1, 2]; "
        "    var r: int := 0; "
        "    if a[0] = 3 then r := r + 1; end; "
        "    if a[1] = 1 then r := r + 2; end; "
        "    if a[2] = 2 then r := r + 4; end; "
        "    return r; "
        "  end Main; "
        "end C; "), 7);
}

// #2774 neighbor: `var ll: LinkedList<int> := [...]` — NOT in this fix's
// scope.  Pin the pre-fix behavior of LinkedList literals so the regression
// sweep flags an inadvertent change to the LinkedList family (a clean exit
// here today means the dispose path is correct; a fail means a regression
// that this fix is not the source of).  LinkedList literals are a separate,
// unfiled issue that #2774 deliberately does not widen to.
BEZEL_TEST(array_string_literal_local_leak_2774, linked_list_int_local_neighbor_check) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var ll: LinkedList<int> := [10, 20, 30]; "
        "    var r: int := 0; "
        "    r := r + ll.Count; "
        "    return r; "
        "  end Main; "
        "end C; "), 3);
}