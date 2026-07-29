// Codegen — issue #1069 (stdlib §4.10 `Set<T>`/`OwningSet<T>`, §5 lazy
// sources, spec §29.2 iteration): a `Set` widened to a pure `IEnumerable<T>`
// was not a vtable-backed enumerator, so the consumer's for-in misread the raw
// backing pointer's first word as a vtable.
//
// Verified on main before the fix (probes): passing a `Set<int>` or an
// `OwningSet<Item>` to an `IEnumerable<T>` PARAMETER and iterating it
// SIGSEGVs ("SEGV on unknown address 0x0, pc points to the zero page" — a
// garbage function-pointer call through the non-vtable first word). The issue
// text recorded the older symptom, "silently yields 0"; either way the value is
// wrong. `wrapAsIEnumerableForTarget` wrapped Array/Stack/Queue/LinkedList/
// user-class sources but had NO Set case, so a Set fell through unwrapped.
//
// Fix — two backings, two treatments (both in wrapAsIEnumerableForTarget, so
// the argument, RETURN and local-decl coercion sites share one rule):
//   * CLASS/interface element — `genericBackingType` declines every class, so
//     the set is `bz_array_t`-backed and that backing IS the dense element
//     array. It therefore takes the plain BORROWING array enumerator: the
//     wrapper shares the source, adds no ownership, and the set's own binder
//     still disposes its elements exactly once. (`OwningSet<T>` requires
//     `where T: class`, so this is the only backing a valid OwningSet has.)
//   * value/record/string element — a generic-key HASH table with no dense
//     element array (exactly why #1068's dense-array fix excluded Set).
//     Snapshot it via the §389 Set lazy source (`bz_lazy_from_set`) and drain
//     that once into a fresh array behind an OWNING enumerator
//     (`bz_lazy_to_array`, which also disposes the lazy source). The SOURCE set
//     is untouched — nothing is retained, released or disposed on it — and the
//     new `outOwningWrap` report tells the ARGUMENT site to register the
//     scope-end iterEnumDispose that frees the snapshot exactly once (the
//     #1923 contract; a RETURN site's caller owns the result by construction).
//
// `SortedSet<T>` is deliberately NOT covered: the snapshot is in hash order and
// yielding a sorted set's elements unordered would silently break §4.18's
// ordered-iteration contract — a worse failure than the current one. Filed
// separately.
//
// fail-without / pass-with: every test below CRASHES on the pre-fix compiler
// (SIGSEGV -> the -999 sentinel); with the fix each returns the asserted VALUE.
// // per stdlib §4.10, §5, spec §29.2 (issue #1069)

#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// -999 on any sanitizer fire (leak / UAF / double-free), else the real exit
// code. Used for the DICT-backed cases, whose snapshot this fix makes the
// caller dispose exactly once.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// ASan without LeakSanitizer (UAF / OOB in emitted loads still trip). Used for
// the CLASS-element ARGUMENT case only: the borrowing enumerator STRUCT that
// every argument-position wrap allocates is not freed after the call — a
// PRE-EXISTING, shared gap, reproduced identically by an `Array<int>` argument
// on unmodified main (32 bytes, calloc in main). It is not introduced here and
// is filed separately; this test pins the VALUE and the absence of the SEGV.
int runCheckedNoLeak(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    return r.ran ? r.exitCode : -999;
}

const char* kItem =
    "module M; class Item "
    "  public V: int := 0; "
    "  public constructor(v: int) this.V := v; end constructor; "
    "  destructor end destructor; "
    "end Item; class C "
    "  public static method Sum(xs: IEnumerable<Item>): int "
    "    var n: int := 0; "
    "    for var x in xs do n := n + x.V; end; "
    "    return n; "
    "  end Sum; ";

const char* kSumInt =
    "module M; class C "
    "  public static method Sum(xs: IEnumerable<int>): int "
    "    var n: int := 0; "
    "    for var x in xs do n := n + x; end; "
    "    return n; "
    "  end Sum; ";
}  // namespace

// ===========================================================================
// stdlib §4.10 + spec §29.2 — a Set as an `IEnumerable<T>` ARGUMENT
// ===========================================================================

// Dict-backed (`Set<int>`). Pre-fix: SIGSEGV in the callee's for-in.
BEZEL_TEST(set_as_ienumerable_1069, dict_backed_set_as_argument_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kSumInt) +
        "public static method Main(): int "
        "  do var s := new Set<int>(); s.Add(5); s.Add(6); "
        "    return C.Sum(s); end; "
        "  return 0; end Main; end C;"), 11);
}

// The source set is untouched by the wrap — still usable after the call, and
// still Count 3 (18 + 3). Pre-fix: SIGSEGV.
BEZEL_TEST(set_as_ienumerable_1069, source_set_intact_after_argument_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kSumInt) +
        "public static method Main(): int "
        "  var r: int := 0; "
        "  do var s: Set<int> := [5, 6, 7]; "
        "    r := C.Sum(s); r := r + s.Count; end; "
        "  return r; end Main; end C;"), 21);
}

// Array-backed (`OwningSet<Item>` — a class element). Pre-fix: SIGSEGV.
// The set still owns and disposes its elements exactly once (the wrap is a
// pure borrow), which the following test pins by observing the source after.
BEZEL_TEST(set_as_ienumerable_1069, class_element_owning_set_as_argument_spec_4_10) {
    BEZEL_EXPECT_EQ(runCheckedNoLeak(std::string(kItem) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<Item>(); "
        "    s.Add(move (new Item(5))); s.Add(move (new Item(6))); "
        "    return C.Sum(s); end; "
        "  return 0; end Main; end C;"), 11);
}

// The owning set is still fully live after being viewed as an IEnumerable:
// 11 (sum) + 2 (Count) = 13. Pre-fix: SIGSEGV.
BEZEL_TEST(set_as_ienumerable_1069, owning_set_intact_after_argument_spec_4_10) {
    BEZEL_EXPECT_EQ(runCheckedNoLeak(std::string(kItem) +
        "public static method Main(): int "
        "  var r: int := 0; "
        "  do using var s: OwningSet<Item> := [new Item(5), new Item(6)]; "
        "    r := C.Sum(s); r := r + s.Count; end; "
        "  return r; end Main; end C;"), 13);
}

// ===========================================================================
// stdlib §4.10 + spec §29.11 — a Set bound to an `IEnumerable<T>` LOCAL
// ===========================================================================

// §29.11 re-iterability: the local is a re-iterable SOURCE, so a second for-in
// replays it — 2 * (5 + 6) = 22. runAotChecked also pins that the snapshot the
// local owns is freed exactly once at scope end. Pre-fix: SIGSEGV.
BEZEL_TEST(set_as_ienumerable_1069, dict_backed_set_as_local_reiterable_spec_29_11) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C public static method Main(): int "
        "  var n: int := 0; "
        "  do var s := new Set<int>(); s.Add(5); s.Add(6); "
        "    var xs: IEnumerable<int> := s; "
        "    for var x in xs do n := n + x; end; "
        "    for var x in xs do n := n + x; end; end; "
        "  return n; end Main; end C;"), 22);
}

// The class-element (array-backed) local form. Pre-fix: SIGSEGV.
BEZEL_TEST(set_as_ienumerable_1069, class_element_set_as_local_spec_29_11) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kItem) +
        "public static method Main(): int "
        "  var n: int := 0; "
        "  do using var s: OwningSet<Item> := [new Item(5), new Item(6)]; "
        "    var xs: IEnumerable<Item> := s; "
        "    for var x in xs do n := n + x.V; end; end; "
        "  return n; end Main; end C;"), 11);
}

// ===========================================================================
// stdlib §4.10 + spec §29 — DIRECT `for`-in over a Set (the issue's title)
// ===========================================================================

// A dict-backed Set iterates via lowerDictBackedSetFor (this already worked and
// must keep working — the fix must not divert it to the new wrap).
BEZEL_TEST(set_as_ienumerable_1069, direct_forin_dict_backed_set_spec_29) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C public static method Main(): int "
        "  var n: int := 0; "
        "  do var s: Set<int> := [5, 6, 7]; "
        "    for var x in s do n := n + x; end; end; "
        "  return n; end Main; end C;"), 18);
}

// A class-element Set iterates via lowerArrayFor. Pre-fix this compiled only
// when the set was built with `new` + `.Add`; over a LITERAL it failed with
// "not yet implemented: codegen for this for statement" (the #1070 root cause
// this issue shares).
BEZEL_TEST(set_as_ienumerable_1069, direct_forin_class_element_set_spec_29) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kItem) +
        "public static method Main(): int "
        "  var n: int := 0; "
        "  do using var s: OwningSet<Item> := [new Item(5), new Item(6)]; "
        "    for var x in s do n := n + x.V; end; end; "
        "  return n; end Main; end C;"), 11);
}

// ===========================================================================
// Regression the fix must NOT disturb
// ===========================================================================

// An `Array<int>` argument still takes the plain borrowing array wrap (the
// `actualIsArray` branch is unchanged; the Set case is a sibling, not a
// replacement). Value-only: the shared enumerator-struct gap noted above.
BEZEL_TEST(set_as_ienumerable_1069, array_as_argument_unchanged_spec_29_2) {
    BEZEL_EXPECT_EQ(run(std::string(kSumInt) +
        "public static method Main(): int "
        "  var r: int := 0; "
        "  do var s := new Array<int>(); s.Add(5); s.Add(6); "
        "    r := C.Sum(s); end; "
        "  return r; end Main; end C;"), 11);
}
