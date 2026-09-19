// Spec-derived regression tests for fix #4099: a `SortedSet<T>` /
// `SortedDictionary<K, V>` widened to `IEnumerable<T>` /
// `IEnumerable<DictionaryEntry<K, V>>` at the argument, RETURN, or
// local-decl-init coercion still silently admitted an eligible-but-not-
// orderable element/key (auto-hashable record or class/interface, no
// dispatchable `CompareTo`) AFTER fix #3976 closed the identical gap for
// the indexer and `for`-in iteration.
//
// AUTHORITY. stdlib §4.17 (v0.295): `SortedDictionary<K, V>` requires
// `K: IComparable<K>` — the SOLE constraint. stdlib §4.18 states the
// identical shape for `SortedSet<T>` (T: IComparable<T>). The widening
// coercion is a SEPARATE C++ translation-unit surface from #3976's fix:
// `wrapAsIEnumerableForTarget` (LlvmBackend_Assign.cpp) serves the
// argument-passing, RETURN-coercion, expression-bodied return, and
// property-getter faces; the local-decl-init face has its own code path
// (LlvmBackend_Setup.cpp's `lowerLocalDecl`) that does NOT route through
// `wrapAsIEnumerableForTarget`. Both surfaces mint the same
// vtable-backed enumerator wrapper the explicit `.GetEnumerator()` and
// `for`-in paths already consult; the predicate the wrap helpers
// consult (`genericBackingType`, in `LlvmBackend_Calls3.cpp`) admits an
// element/key via its hash-only branches regardless of ordering, exactly
// the gap #3976 closed.
//
// THE FIX (this file). The shared authority
// `Emitter::reportSortedElementNeedsCompareTo` (LlvmBackend_Calls3.cpp,
// fix #3976) is consulted BEFORE the wrap is built at every coercion
// face: argument (in `wrapAsIEnumerableForTarget`), RETURN (same), and
// local-decl-init (in `LlvmBackend_Setup.cpp`). The wrap mint is skipped
// on a rejected surface — a well-formed no-op — matching how the indexer
// returns a poison value on the same predicate. The fix reuses ONE
// authority rather than adding a third inline copy of the predicate
// (LANE_RULES §15).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string out(const std::string& src) { return runAotOut(src); }

bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `Pt`: auto-hashable record (§9.2), NO `CompareTo` — the issue's own shape.
// `Cmp`: auto-hashable record WITH `CompareTo` — an accept-side control.
const char* PRELUDE =
    "module M; "
    "record Pt "
    "  public property X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "end Pt; "
    "record Cmp "
    "  public property X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public method CompareTo(other: Cmp): int return X - other.X; "
    "    end CompareTo; "
    "end Cmp; ";

const char* NEEDLE_SET =
    "requires the element type to implement `IComparable<T>`";
const char* NEEDLE_DICT =
    "requires the key type to implement `IComparable<K>`";

}  // namespace

// ── THE ISSUE'S OWN REPRO, VERBATIM IN SHAPE ────────────────────────────────

// Argument-coercion site: SortedDictionary<Pt, int> passed to
// IEnumerable<DictionaryEntry<Pt, int>> sink.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           argument_coercion_sorteddictionary_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(PRELUDE) +
            "class C "
            "  public static method Sink(items: IEnumerable<DictionaryEntry<Pt, int>>): int "
            "    return 0; "
            "  end Sink; "
            "  public static method Run(d: SortedDictionary<Pt, int>): int "
            "    return Sink(d); "
            "  end Run; "
            "  public static method Main(): int return 0; end Main; "
            "end C;",
        NEEDLE_DICT));
}

// Argument-coercion site: SortedSet<Pt> passed to IEnumerable<Pt> sink.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           argument_coercion_sortedset_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(PRELUDE) +
            "class C "
            "  public static method Sink(items: IEnumerable<Pt>): int "
            "    return 0; "
            "  end Sink; "
            "  public static method Run(s: SortedSet<Pt>): int "
            "    return Sink(s); "
            "  end Run; "
            "  public static method Main(): int return 0; end Main; "
            "end C;",
        NEEDLE_SET));
}

// ── RETURN coercion site ────────────────────────────────────────────────────

// SortedDictionary returned as IEnumerable<DictionaryEntry<K, V>>.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           return_coercion_sorteddictionary_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(PRELUDE) +
            "class C "
            "  public static method Run(d: SortedDictionary<Pt, int>): IEnumerable<DictionaryEntry<Pt, int>> "
            "    return d; "
            "  end Run; "
            "  public static method Main(): int return 0; end Main; "
            "end C;",
        NEEDLE_DICT));
}

// SortedSet returned as IEnumerable<T>.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           return_coercion_sortedset_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(PRELUDE) +
            "class C "
            "  public static method Run(s: SortedSet<Pt>): IEnumerable<Pt> "
            "    return s; "
            "  end Run; "
            "  public static method Main(): int return 0; end Main; "
            "end C;",
        NEEDLE_SET));
}

// Expression-bodied return face (also serves as a sanity coverage of the
// `wrapAsIEnumerableForTarget` `ownLazyPipeline=true` dispatch — a lazy
// pipeline result, which the wrap handles identically, would also be
// rejected for the same predicate).
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           arrow_return_coercion_sortedset_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(PRELUDE) +
            "class C "
            "  public static method Run(s: SortedSet<Pt>): IEnumerable<Pt> => s; "
            "  public static method Main(): int return 0; end Main; "
            "end C;",
        NEEDLE_SET));
}

// ── Local-decl-init coercion site ───────────────────────────────────────────

// SortedDictionary assigned to an IEnumerable<DictionaryEntry<K, V>> local.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           local_decl_init_sorteddictionary_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(PRELUDE) +
            "class C "
            "  public static method Run(d: SortedDictionary<Pt, int>): int "
            "    var e: IEnumerable<DictionaryEntry<Pt, int>> := d; "
            "    return 0; "
            "  end Run; "
            "  public static method Main(): int return 0; end Main; "
            "end C;",
        NEEDLE_DICT));
}

// SortedSet assigned to an IEnumerable<T> local.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           local_decl_init_sortedset_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(PRELUDE) +
            "class C "
            "  public static method Run(s: SortedSet<Pt>): int "
            "    var e: IEnumerable<Pt> := s; "
            "    return 0; "
            "  end Run; "
            "  public static method Main(): int return 0; end Main; "
            "end C;",
        NEEDLE_SET));
}

// ── Accept-side sweep (LANE_RULES §15): every legitimate shape must still ──
// ── compile AND behave correctly through the widening coercion. ─────────────

// Plain Dictionary<K, V> widened at the argument site — unaffected. The
// `where K: IEquatable<K>` constraint (§4.8) admits any key with
// structural equality, which is the right contract for an unordered map.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           accept_dictionary_argument_coercion_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(PRELUDE) +
            "class C "
            "  public static method Main(): int "
            "    var d := new Dictionary<Pt, int>(); "
            "    d.Add(new Pt(1), 10); d.Add(new Pt(2), 20); "
            "    var sum: int := 0; "
            "    for var e in d do sum := sum + e.Value; end; "
            "    Console.WriteLine(sum); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("30"));
}

// SortedDictionary WITH a CompareTo-eligible key — unaffected.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           accept_sorteddictionary_argument_coercion_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(PRELUDE) +
            "class C "
            "  public static method Main(): int "
            "    var sd := new SortedDictionary<Cmp, int>(); "
            "    sd.Add(new Cmp(2), 20); sd.Add(new Cmp(1), 10); "
            "    var first := 0; "
            "    for var e in sd do first := e.Value; break; end; "
            "    Console.WriteLine(first); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("10"));
}

// SortedSet WITH a CompareTo-eligible element — unaffected.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           accept_sortedset_argument_coercion_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(PRELUDE) +
            "class C "
            "  public static method Main(): int "
            "    var s := new SortedSet<Cmp>(); "
            "    s.Add(new Cmp(3)); s.Add(new Cmp(1)); s.Add(new Cmp(2)); "
            "    var first := 0; "
            "    for var e in s do first := e.X; break; end; "
            "    Console.WriteLine(first); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("1"));
}

// SortedDictionary RETURNED as `IEnumerable<DictionaryEntry<K, V>>` —
// the accept-side mirror of the rejected RETURN coercion. Uses
// `SortedDictionary<int, int>` because the wrap helper at
// `LlvmBackend_Assign.cpp`'s `dictEntryWrapShapeSupported` declines
// record key/value shapes (unchanged by #4099 — its SortedDictionary arm
// keeps the v0.283 narrow answer, mirroring `dictSnapshotValueDisposers`'s
// own SortedDictionary arm). A Cmp-keyed entry was never wired through
// the IEnumerable<DictionaryEntry<K,V>> coercion in the first place.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           accept_sorteddictionary_return_coercion_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(PRELUDE) +
            "class C "
            "  public static method Make(): IEnumerable<DictionaryEntry<int, int>> "
            "    var sd := new SortedDictionary<int, int>(); "
            "    sd.Add(1, 10); sd.Add(2, 20); "
            "    return sd; "
            "  end Make; "
            "  public static method Main(): int "
            "    var e := Make(); "
            "    var sum: int := 0; "
            "    for var x in e do sum := sum + x.Value; end; "
            "    Console.WriteLine(sum); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("30"));
}

// SortedSet RETURNED — the accept-side mirror of the rejected RETURN
// coercion. Reuses the `Run` -> `Main` shape so the RETURN coercion is
// the actual tested face (not a `new` shape that bypasses it).
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           accept_sortedset_return_coercion_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(PRELUDE) +
            "class C "
            "  public static method Build(s: SortedSet<Cmp>): SortedSet<Cmp> "
            "    return s; "
            "  end Build; "
            "  public static method Main(): int "
            "    var s := new SortedSet<Cmp>(); "
            "    s.Add(new Cmp(5)); s.Add(new Cmp(3)); "
            "    var t := Build(s); "
            "    Console.WriteLine(t.Count); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("2"));
}

// SortedDictionary bound to a local — the accept-side mirror of the
// rejected local-decl-init. Uses `SortedDictionary<int, int>` because
// the wrap helper at `LlvmBackend_Assign.cpp`'s `dictEntryWrapShapeSupported`
// declines record key/value shapes (its own gate — unchanged by #4099 —
// mirrors `dictSnapshotValueDisposers`' coverage, and that helper's
// SortedDictionary arm keeps the v0.283 narrow answer; a Cmp-keyed entry
// was never wired through the IEnumerable<DictionaryEntry<K,V>> coercion
// in the first place).
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           accept_sorteddictionary_local_decl_init_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(PRELUDE) +
            "class C "
            "  public static method Main(): int "
            "    var sd := new SortedDictionary<int, int>(); "
            "    sd.Add(2, 20); sd.Add(1, 10); "
            "    var e: IEnumerable<DictionaryEntry<int, int>> := sd; "
            "    var sum: int := 0; "
            "    for var x in e do sum := sum + x.Value; end; "
            "    Console.WriteLine(sum); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("30"));
}

// SortedSet bound to a local — the accept-side mirror of the rejected
// local-decl-init.
BEZEL_TEST(sorted_sorteddict_ienumerable_coercion_compareto_4099,
           accept_sortedset_local_decl_init_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(PRELUDE) +
            "class C "
            "  public static method Main(): int "
            "    var s := new SortedSet<Cmp>(); "
            "    s.Add(new Cmp(3)); s.Add(new Cmp(1)); s.Add(new Cmp(2)); "
            "    var e: IEnumerable<Cmp> := s; "
            "    var first := 0; "
            "    for var x in e do first := x.X; break; end; "
            "    Console.WriteLine(first); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("1"));
}
