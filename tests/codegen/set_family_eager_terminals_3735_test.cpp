// Codegen — stdlib §4.10 / §4.18 / §5 (v0.286), issue 3735: the §5 EAGER
// terminal operators on a DICT-BACKED Set-family receiver.
//
// WHAT WAS BROKEN. `tryLowerCollectionInstanceCall`
// (src/codegen/LlvmBackend_Calls.cpp) routes a §5 terminal
// (Sum/Min/Max/Average/Aggregate/Any/All/Count(pred)/First/Last/Single/
// ElementAt/*OrNull) over a lazy, LinkedList, Range, vtable-IEnumerable or
// string source through ONE materialize-then-run path
// (`lowerToLazyEnumerator` + `bz_lazy_to_array`, then the concrete-array
// terminal lowerings). A `Set<T>`/`OwningSet<T>`/`SortedSet<T>` receiver
// whose element takes the generic-key HASH backing (record / primitive /
// enum / choice / string / decimal, and — since #2773 — a class element on
// `SortedSet`) was NOT on that list: its `bz_dict_t*` fell into the
// dedicated dict-backed member block, which covers only §4.10/§4.18's
// native surface (Add/TryAdd/Contains/Remove/Count()/IsEmpty/Clear/the
// *With family/SortedSet's First-Last) and `return nullptr`s for everything
// else. That null reached the caller as `internal: lowerExpr returned null
// for interpolated-string expression part — #1386` inside a
// `Console.WriteLine($"…")`, and as the SILENT #2434 zero-degrade when
// assigned to a plain local first — measured on this tree:
// `Set<int>{30,10,20}.Sum()` printed 0 where `Array<int>` over the same
// elements prints 60.
//
// WHAT THE FIX IS. One term (`setRecvTerm`) added to the SAME
// materialize-then-run disjunction the five other source families use — no
// new per-operator arms (issue 2726's commit body records that a family-wide
// registry move of the Set heads regressed 30+ tests). `Contains`, no-arg
// `Count` and `SortedSet`'s no-arg `First`/`Last` are deliberately excluded
// and keep their `bz_dict_*` arms; rows below pin that.
//
// ORDER IS LOAD-BEARING and differs by head, so the element-access and
// fold rows assert an ORDER-SENSITIVE value (First/Last/ElementAt, and an
// `Aggregate` that builds a decimal-place-per-element number), never a sum
// that would pass on any permutation:
//   * stdlib §4.10: "Iteration over a `Set<T>` yields elements in
//     **insertion order**" — 30,10,20 stays 30,10,20.
//   * stdlib §4.18: "A comparison-ordered set: membership by
//     `CompareTo`-equality, iteration in ascending order." — 30,10,20
//     becomes 10,20,30.
//
// REFUTATION recorded here as controls: issue 3735's title says "and likely
// `OwningSet<T>`". Measured, `OwningSet<T>` NEVER reproduced — §4.10
// declares it `where T: class`, and `genericBackingType`'s `isSet`
// exclusion (LlvmBackend_Calls3.cpp) sends every class/interface element of
// `Set`/`OwningSet` to the ARRAY-backed set representation, which was
// already on the working path. The `owningset_*` and `set_class_element_*`
// rows below pass on BOTH sides of this fix and exist to pin that.
//
// Every value row uses `runAotOut` (the printed value, never an exit code);
// the leak rows use `runAotChecked` and are LOOP-AMPLIFIED to 200 iterations
// per lane rules §3.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// `Box: IComparable<Box>` — the class element shared by the SortedSet and
// OwningSet rows. `Pt` is the record element (auto-hashable, §9.2).
const char* PRELUDE =
    "module M;\n"
    "import System;\n"
    "record Pt(X: int, Y: int);\n"
    "class Box: IComparable<Box>\n"
    "  public Value: int := 0;\n"
    "  public constructor(v: int) Value := v; end;\n"
    "  public method CompareTo(other: Box): int\n"
    "    return Value - other.Value;\n"
    "  end CompareTo;\n"
    "end Box;\n";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C\n"
           "  public static method Main(): int\n" +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

// A `Set<int>` built in the issue's own order — 30, 10, 20.
const char* SET_INT =
    "    using var s := new Set<int>();\n"
    "    s.Add(30); s.Add(10); s.Add(20);\n";

// The same elements in a `SortedSet<int>`.
const char* SORTED_INT =
    "    using var s := new SortedSet<int>();\n"
    "    s.Add(30); s.Add(10); s.Add(20);\n";

}  // namespace

// ===========================================================================
// Issue 3735's OWN repro, both shapes. The plain-local shape is the SILENT
// #2434 zero (`0` where 60 is required); the interpolation shape is the
// #1386 ICE, which failed the compile outright.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735, issue_repro_sum_via_plain_local) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    var total := s.Sum();\n"
                       "    Console.WriteLine(total.ToString());")),
        "60");
}

BEZEL_TEST(set_family_eager_terminals_3735, issue_repro_sum_in_interpolation) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"sum={s.Sum()}\");")),
        "sum=60");
}

// The issue's control: an `Array<int>` over the same elements always worked.
// Kept beside the repro so a regression in the shared array-terminal
// lowering is distinguishable from a regression in the new Set route.
BEZEL_TEST(set_family_eager_terminals_3735, array_control_still_60) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    var a: Array<int> := [30, 10, 20];\n"
                       "    Console.WriteLine($\"sum={a.Sum()}\");")),
        "sum=60");
}

// ===========================================================================
// Set<int> — the aggregation family (stdlib §5 / §5.1).
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735, set_int_sum_min_max) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"{s.Sum()},{s.Min()},"
                       "{s.Max()}\");")),
        "60,10,30");
}

BEZEL_TEST(set_family_eager_terminals_3735, set_int_average) {
    // stdlib §5.1: `Average(): double` for an int source. Rendered through
    // `int(…)` so the row asserts the numeric value, not `double.ToString`'s
    // exponent formatting.
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    var avg := s.Average();\n"
                       "    Console.WriteLine($\"{int(avg)}\");")),
        "20");
}

BEZEL_TEST(set_family_eager_terminals_3735, set_int_selector_aggregations) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"{s.Sum(lambda(x: int) => x * 2)},"
                       "{s.Min(lambda(x: int) => x)},"
                       "{s.Max(lambda(x: int) => x)}\");")),
        "120,10,30");
}

// ORDER-SENSITIVE: `Aggregate` builds one decimal place per element, so the
// result is 301020 for §4.10's insertion order and would be a different
// number for any permutation.
BEZEL_TEST(set_family_eager_terminals_3735, set_int_aggregate_insertion_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"{s.Aggregate(0, "
                       "lambda(a: int, x: int) => a * 100 + x)}\");")),
        "301020");
}

// ===========================================================================
// Set<int> — quantifiers and element access. First/Last/ElementAt assert
// §4.10's INSERTION order directly (30, 10, 20), not a permutation-blind
// count.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735, set_int_quantifiers) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"{s.Any()},"
                       "{s.Any(lambda(x: int) => x > 25)},"
                       "{s.Any(lambda(x: int) => x > 99)},"
                       "{s.All(lambda(x: int) => x > 5)},"
                       "{s.All(lambda(x: int) => x > 15)}\");")),
        "True,True,False,True,False");
}

BEZEL_TEST(set_family_eager_terminals_3735,
           set_int_element_access_insertion_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"{s.First()},{s.Last()},"
                       "{s.ElementAt(0)},{s.ElementAt(1)},"
                       "{s.ElementAt(2)}\");")),
        "30,20,30,10,20");
}

BEZEL_TEST(set_family_eager_terminals_3735,
           set_int_predicate_element_access) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"{s.First(lambda(x: int) => x > 15)},"
                       "{s.Last(lambda(x: int) => x > 15)},"
                       "{s.Single(lambda(x: int) => x = 10)}\");")),
        "30,20,10");
}

BEZEL_TEST(set_family_eager_terminals_3735, set_int_ornull_family) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"{s.FirstOrNull()},"
                       "{s.LastOrNull()},"
                       "{s.SingleOrNull(lambda(x: int) => x = 20)}\");")),
        "30,20,20");
}

BEZEL_TEST(set_family_eager_terminals_3735,
           set_int_ornull_family_empty_set_is_null) {
    // The *OrNull contract on an EMPTY source: null, not a degraded zero.
    // This row also distinguishes "the terminal ran and found nothing" from
    // the pre-fix "the slot was zero-initialised" — the pre-fix compiler
    // printed nothing at all here (the whole WriteLine failed to lower).
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new Set<int>();\n"
                       "    var f := s.FirstOrNull();\n"
                       "    Console.WriteLine(f = null);")),
        "True");
}

// ===========================================================================
// The three EXCLUDED members keep their dedicated `bz_dict_*` arms. If the
// exclusion ever drifts these still pass (the snapshot path answers them
// correctly too) — they are here to pin the intent, and `Contains` in
// particular must keep the element's §8.3 hash/equality rather than a linear
// `==` scan over a snapshot.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735, set_int_count_and_contains) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SET_INT) +
                       "    Console.WriteLine($\"{s.Count()},{s.Contains(10)},"
                       "{s.Contains(99)}\");")),
        "3,True,False");
}

// ===========================================================================
// Set<string> — the RuntimeManaged element row (§28.11). The snapshot
// BORROWS the set's own `+1` per element; the value rows prove the pointers
// are read correctly, the 200x row below proves nothing is double-released.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735, set_string_terminals) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new Set<string>();\n"
                       "    s.Add(\"pear\"); s.Add(\"apple\"); s.Add(\"kiwi\");\n"
                       "    Console.WriteLine($\"{s.Sum(lambda(x: string) => x.Length)},"
                       "{s.Max(lambda(x: string) => x.Length)},"
                       "{s.First()},{s.Last()},"
                       "{s.Any(lambda(x: string) => x.Length = 5)}\");")),
        "13,5,pear,kiwi,True");
}

// ===========================================================================
// Set<record> — the record element row (#343's generic-key backing). The
// element is a 2-field struct, so this also exercises the snapshot's element
// STRIDE (collectionElemSize's struct branch) rather than a scalar.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735, set_record_terminals) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new Set<Pt>();\n"
                       "    s.Add(new Pt(1, 2)); s.Add(new Pt(3, 4));\n"
                       "    s.Add(new Pt(5, 6));\n"
                       "    Console.WriteLine($\"{s.Sum(lambda(p: Pt) => p.X + p.Y)},"
                       "{s.First().X},{s.Last().Y},{s.ElementAt(1).X}\");")),
        "21,1,6,3");
}

// ===========================================================================
// SortedSet<int> — the SAME operators, with §4.18's ASCENDING order. Every
// order-dependent row here differs from its Set<int> twin above, which is
// exactly what makes them evidence that #3711's sorted snapshot (not
// `bz_lazy_from_set`'s hash order) is what the terminals read.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735, sortedset_int_aggregations) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SORTED_INT) +
                       "    Console.WriteLine($\"{s.Sum()},{s.Min()},"
                       "{s.Max()},{int(s.Average())}\");")),
        "60,10,30,20");
}

BEZEL_TEST(set_family_eager_terminals_3735,
           sortedset_int_element_access_ascending) {
    // §4.18 ascending: 10, 20, 30 — the Set<int> twin above reads 30, 20, 30,
    // 10, 20 for the identical expression.
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SORTED_INT) +
                       "    Console.WriteLine($\"{s.First()},{s.Last()},"
                       "{s.ElementAt(0)},{s.ElementAt(1)},"
                       "{s.ElementAt(2)}\");")),
        "10,30,10,20,30");
}

BEZEL_TEST(set_family_eager_terminals_3735, sortedset_int_aggregate_ascending) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SORTED_INT) +
                       "    Console.WriteLine($\"{s.Aggregate(0, "
                       "lambda(a: int, x: int) => a * 100 + x)}\");")),
        "102030");
}

BEZEL_TEST(set_family_eager_terminals_3735, sortedset_int_quantifiers) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SORTED_INT) +
                       "    Console.WriteLine($\"{s.Any()},"
                       "{s.Any(lambda(x: int) => x > 25)},"
                       "{s.All(lambda(x: int) => x > 5)},"
                       "{s.Count(lambda(x: int) => x > 15)}\");")),
        "True,True,True,2");
}

BEZEL_TEST(set_family_eager_terminals_3735, sortedset_int_ornull_ascending) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SORTED_INT) +
                       "    Console.WriteLine($\"{s.FirstOrNull()},"
                       "{s.LastOrNull()},"
                       "{s.SingleOrNull(lambda(x: int) => x = 20)}\");")),
        "10,30,20");
}

// `First()`/`Last()` with NO argument keep the dedicated
// `bz_dict_min_key_gen`/`bz_dict_max_key_gen` arm (the exclusion), and must
// agree with what the snapshot path produces for `ElementAt(0)`/
// `ElementAt(2)` — the two surfaces disagreeing is exactly the property
// #2471/#2472 established they must not.
BEZEL_TEST(set_family_eager_terminals_3735,
           sortedset_dict_arm_and_snapshot_agree) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(std::string(SORTED_INT) +
                       "    Console.WriteLine($\"{s.First() = s.ElementAt(0)},"
                       "{s.Last() = s.ElementAt(2)}\");")),
        "True,True");
}

// ===========================================================================
// SortedSet<Box> — a CLASS element (dict-backed since #2773). Ordering is by
// `CompareTo`, so the ORDER-SENSITIVE fold is the evidence.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735,
           sortedset_class_element_terminals_ascending) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new SortedSet<Box>();\n"
            "    using var b1 := new Box(30);\n"
            "    using var b2 := new Box(10);\n"
            "    using var b3 := new Box(20);\n"
            "    s.Add(b1); s.Add(b2); s.Add(b3);\n"
            "    Console.WriteLine($\"{s.Sum(lambda(b: Box) => b.Value)},"
            "{s.Aggregate(0, lambda(a: int, b: Box) => a * 100 + b.Value)},"
            "{s.ElementAt(1).Value},"
            "{s.Max(lambda(b: Box) => b.Value)}\");")),
        "60,102030,20,30");
}

// ===========================================================================
// REFUTATION CONTROLS — these pass on BOTH sides of the fix. `OwningSet<T>`
// is `where T: class` (stdlib §4.10) and a class/interface element of
// `Set`/`OwningSet` takes the ARRAY-backed set representation, so neither
// ever reached the dict block's `return nullptr`. Issue 3735's "and likely
// OwningSet<T>" is refuted; these rows pin that it stays working.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735,
           owningset_class_element_terminals_control) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new OwningSet<Box>();\n"
            "    s.Add(new Box(30)); s.Add(new Box(10)); s.Add(new Box(20));\n"
            "    Console.WriteLine($\"{s.Sum(lambda(b: Box) => b.Value)},"
            "{s.Count()},{s.First().Value},"
            "{s.Any(lambda(b: Box) => b.Value > 25)},"
            "{s.All(lambda(b: Box) => b.Value > 5)}\");")),
        "60,3,30,True,True");
}

BEZEL_TEST(set_family_eager_terminals_3735,
           set_class_element_terminals_control) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new Set<Box>();\n"
            "    using var b1 := new Box(30);\n"
            "    using var b2 := new Box(10);\n"
            "    using var b3 := new Box(20);\n"
            "    s.Add(b1); s.Add(b2); s.Add(b3);\n"
            "    Console.WriteLine($\"{s.Sum(lambda(b: Box) => b.Value)},"
            "{s.Count()},{s.First().Value}\");")),
        "60,3,30");
}

// ===========================================================================
// OTHER RECEIVER FAMILIES ARE UNTOUCHED. The new term is `setRecvTerm`, a
// conjunct on the dict-backed Set heads only; every other family reaching
// this dispatch keeps whatever path it had. One program, all of them.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735, other_receiver_families_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a: Array<int> := [30, 10, 20];\n"
            "    using var st := new Stack<int>();\n"
            "    st.Push(30); st.Push(10);\n"
            "    using var q := new Queue<int>();\n"
            "    q.Enqueue(30); q.Enqueue(10);\n"
            "    using var ll := new LinkedList<int>();\n"
            "    ll.AddLast(30); ll.AddLast(10);\n"
            "    var lz := a.Where(lambda(x: int) => x > 15);\n"
            "    var sAny := \"abc\".Any(lambda(c: char) => c = 'b');\n"
            "    var rg := 1..<5;\n"
            "    Console.WriteLine($\"{a.Sum()},{a.IndexOf(10)},{st.Sum()},"
            "{q.Sum()},{ll.Sum()},{lz.Sum()},{sAny},{rg.Sum()}\");")),
        "60,1,40,40,40,50,True,10");
}

// ===========================================================================
// LEAK ROWS — `runAotChecked` (ASan/LSan), LOOP-AMPLIFIED to 200 iterations
// per lane rules §3. The snapshot the terminals drain is a fresh owned
// `bz_array_t` freed once at the terminal's return with a NULL element
// disposer: every element is BORROWED from the set, which releases its own
// `+1` (string) or disposes its owned elements (OwningSet) exactly once at
// its scope end.
// ===========================================================================

BEZEL_TEST(set_family_eager_terminals_3735,
           set_int_terminals_leak_free_200x) {
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var s := new Set<int>();\n"
        "      s.Add(30); s.Add(10); s.Add(20);\n"
        "      total := total + s.Sum() + s.Min() + s.Max() + s.ElementAt(1);\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // (60 + 10 + 30 + 10) * 200 = 22000.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=22000\n"));
}

BEZEL_TEST(set_family_eager_terminals_3735,
           set_string_terminals_leak_free_200x) {
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var s := new Set<string>();\n"
        "      s.Add(\"pear\"); s.Add(\"apple\"); s.Add(\"kiwi\");\n"
        "      total := total + s.Sum(lambda(x: string) => x.Length)\n"
        "                     + s.First().Length + s.Last().Length;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // (13 + 4 + 4) * 200 = 4200.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=4200\n"));
}

BEZEL_TEST(set_family_eager_terminals_3735,
           sortedset_class_elements_terminals_leak_free_200x) {
    // Elements are BORROWED by the SortedSet (stdlib §28.11's borrow-slot
    // row); each `Box` is disposed exactly once by its own `using var`, never
    // by the set and never by the terminal's snapshot.
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var s := new SortedSet<Box>();\n"
        "      using var b1 := new Box(30);\n"
        "      using var b2 := new Box(10);\n"
        "      using var b3 := new Box(20);\n"
        "      s.Add(b1); s.Add(b2); s.Add(b3);\n"
        "      total := total + s.Sum(lambda(b: Box) => b.Value)\n"
        "                     + s.ElementAt(1).Value;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // (60 + 20) * 200 = 16000.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=16000\n"));
}

BEZEL_TEST(set_family_eager_terminals_3735,
           owningset_class_elements_terminals_leak_free_200x) {
    // The refutation control under ASan: `OwningSet<Box>` OWNS its elements
    // and disposes each exactly once at its own scope end; the terminals run
    // over the array-backed representation and add no ownership.
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var s := new OwningSet<Box>();\n"
        "      s.Add(new Box(30)); s.Add(new Box(10)); s.Add(new Box(20));\n"
        "      total := total + s.Sum(lambda(b: Box) => b.Value)\n"
        "                     + s.First().Value;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // (60 + 30) * 200 = 18000.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=18000\n"));
}
