// Codegen — stdlib §4.18 / §5 (v0.286), issue 3730: value correctness for
// the stdlib §5 operator surface now typed on a DIRECT `SortedSet<T>`
// receiver (the sema fix is `isArrLike` admitting `SortedSet` in
// `ExprTypecheck_Calls.cpp`). The codegen lowering itself was already fixed
// by #3711 (`Emitter::lowerToLazyEnumerator`'s SortedSet arm feeding
// `sortedSetSnapshotToSortedArray`'s §4.18 ascending snapshot); this file
// pins that the SEMA half now reaches that lowering — before this fix,
// `ctx_.exprType(ce)` stayed null on every row here and every one degraded
// via #2434 to a zero-initialized value.
//
// The rejection half (members with no lowering, either because §4.18 never
// declares them or because this compiler's direct-eager-operator dispatch
// had no SortedSet arm — issue 3735) is pinned in
// tests/sema/sortedset_isarrlike_3730_test.cpp; this file is VALUE
// correctness for what DOES work.
//
// fix #3735 (v0.286) supplied that missing eager-terminal dispatch for every
// dict-backed Set-family receiver, so the second rejection category is gone
// (except `Reverse`, retargeted to issue 3763). Its own value grid — the §5
// eager terminals across Set / OwningSet / SortedSet — lives in
// tests/codegen/set_family_eager_terminals_3735_test.cpp; the LAZY / ToArray
// rows below are unaffected by it and stay here.
//
// `runAotOut` rows assert the printed VALUE, never an exit code or a bare
// "it compiled" — per lane rules §3, an oracle that cannot observe the
// effect cannot certify the path.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& body) {
    return std::string("module M;\n"
                        "import System;\n"
                        "class C\n"
                        "  public static method Main(): int\n") +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

}  // namespace

// ===========================================================================
// issue 3730's own repro (measured): a direct SortedSet<int> receiver's
// ToArray() came back empty (`sortedToArray=0`) while the identical Set<int>
// shape and for-in over the very same SortedSet were both correct — proving
// the set WAS populated and only the §5 path was empty. This is also the
// program the known-gap row in
// tests/codegen/oftype_removal_completeness_3440_test.cpp flips for.
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730, issue_repro_direct_receiver_toarray) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var p := new Set<int>();\n"
                        "    p.Add(3); p.Add(1); p.Add(2);\n"
                        "    using var pt := p.ToArray();\n"
                        "    var ptc := pt.Count;\n"
                        "    Console.WriteLine($\"setToArray={ptc}\");\n"
                        "    using var s := new SortedSet<int>();\n"
                        "    s.Add(3); s.Add(1); s.Add(2);\n"
                        "    using var st := s.ToArray();\n"
                        "    var stc := st.Count;\n"
                        "    Console.WriteLine($\"sortedToArray={stc}\");\n"
                        "    var n := 0;\n"
                        "    for var x in s do n := n + x; end;\n"
                        "    Console.WriteLine($\"forin={n}\");")),
        "setToArray=3\nsortedToArray=3\nforin=6");
}

// ===========================================================================
// ToArray() sequence is ASCENDING (stdlib §4.18), asserted via the actual
// joined element sequence — never a count or a sum, either of which would
// pass on a permuted order.
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730, toarray_ascending_sequence) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new SortedSet<int>();\n"
                        "    s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n"
                        "    using var arr := s.ToArray();\n"
                        "    var acc := \"\";\n"
                        "    for var x in arr do acc := acc + $\"{x},\"; end;\n"
                        "    Console.WriteLine(acc);")),
        "10,20,30,40,");
}

// ===========================================================================
// Where/Select/Count()/First()/Last on a direct receiver, one program.
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730, where_select_count_first_last) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new SortedSet<int>();\n"
            "    s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n"
            "    using var w := s.Where(lambda(x) => x > 15).ToArray();\n"
            "    var wAcc := \"\";\n"
            "    for var x in w do wAcc := wAcc + $\"{x},\"; end;\n"
            "    using var sel := s.Select(lambda(x) => x * 2).ToArray();\n"
            "    var selAcc := \"\";\n"
            "    for var x in sel do selAcc := selAcc + $\"{x},\"; end;\n"
            "    Console.WriteLine($\"where={wAcc} select={selAcc} "
            "count={s.Count()} first={s.First()} last={s.Last()}\");")),
        "where=20,30,40, select=20,40,60,80, count=4 first=10 last=40");
}

// ===========================================================================
// Contains works correctly on a direct receiver — the value half of the
// sema test's "no spurious hashable reject" row.
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730, contains_returns_correct_bool) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new SortedSet<int>();\n"
                        "    s.Add(30); s.Add(10); s.Add(20);\n"
                        "    Console.WriteLine($\"{s.Contains(10)},"
                        "{s.Contains(99)}\");")),
        "True,False");
}

// ===========================================================================
// The neighbouring-`using var` leak, from the issue's comment: the #2434
// degrade on a SortedSet local displaced the disposal of a correctly-lowered
// `using var` bound just above it — the Set's own materialized array leaked.
// LOOP-AMPLIFIED to 200 iterations (lane rules §3: a one-shot LSan row passes
// on both sides because the last allocation is still stack-reachable at
// exit). Must be ASan/LSan-clean now that the SortedSet line no longer
// degrades.
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730,
           neighbouring_using_var_not_leaked_200x) {
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var p := new Set<int>();\n"
        "      p.Add(3); p.Add(1); p.Add(2);\n"
        "      using var pt := p.ToArray();\n"
        "      total := total + pt.Count;\n"
        "      using var s := new SortedSet<int>();\n"
        "      s.Add(3);\n"
        "      using var st := s.ToArray();\n"
        "      total := total + st.Count;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=800\n"));
}

// ===========================================================================
// string elements: borrowed slots (the SortedSet's own bz_string* +1),
// released once by the set at its own scope end — never by the materialized
// array. LOOP-AMPLIFIED to 200 iterations.
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730, string_elements_leak_free_200x) {
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var s := new SortedSet<string>();\n"
        "      s.Add(\"pear\"); s.Add(\"apple\"); s.Add(\"kiwi\");\n"
        "      using var arr := s.ToArray();\n"
        "      total := total + arr.Count;\n"
        "      using var w := s.Where(lambda(x) => x.Length > 4).ToArray();\n"
        "      total := total + w.Count;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // Per iteration: arr.Count=3, w.Count=1 ("apple" is the only >4-length
    // element) -> 4/iteration * 200 = 800.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=800\n"));
}

// ===========================================================================
// Ordering is preserved for string elements too (stdlib §4.18's ascending
// guarantee is lexicographic for string, per the #2550 comparator).
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730, string_elements_ascending) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new SortedSet<string>();\n"
                        "    s.Add(\"pear\"); s.Add(\"apple\"); s.Add(\"kiwi\");\n"
                        "    using var arr := s.ToArray();\n"
                        "    var acc := \"\";\n"
                        "    for var x in arr do acc := acc + $\"{x},\"; end;\n"
                        "    Console.WriteLine(acc);")),
        "apple,kiwi,pear,");
}

// ===========================================================================
// Class elements: `Box: IComparable<Box>` is a legal SortedSet<T> element
// (measured on this tree — #3730's own fix commit corrects the stray claim
// in #3711's commit body that class elements are excluded by #2773; #2773's
// OWN test file, tests/codegen/sorted_set_class_element_2773_test.cpp,
// documents the admission as fixed and current). Ownership: `s.Add(b1)`
// stores a BORROW (SortedSet is a borrowing-family collection, stdlib
// §28.11); `b1`/`b2`/`b3` are disposed once each by their own `using var`
// scope, not by the set. LOOP-AMPLIFIED to 200 iterations.
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730, class_elements_leak_free_200x) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "class Box: IComparable<Box>\n"
        "  public Value: int := 0;\n"
        "  public constructor(v: int) Value := v; end;\n"
        "  public method CompareTo(other: Box): int\n"
        "    return Value - other.Value;\n"
        "  end CompareTo;\n"
        "end Box;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var s := new SortedSet<Box>();\n"
        "      using var b1 := new Box(30);\n"
        "      using var b2 := new Box(10);\n"
        "      using var b3 := new Box(20);\n"
        "      s.Add(b1); s.Add(b2); s.Add(b3);\n"
        "      using var arr := s.ToArray();\n"
        "      for var x in arr do total := total + x.Value; end;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // (30+10+20) * 200 = 12000.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=12000\n"));
}

// ===========================================================================
// Class elements are ALSO returned in ascending CompareTo order — the same
// property proven for int/string above, now over a reference element.
// ===========================================================================
BEZEL_TEST(sortedset_isarrlike_3730, class_elements_ascending) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "class Box: IComparable<Box>\n"
        "  public Value: int := 0;\n"
        "  public constructor(v: int) Value := v; end;\n"
        "  public method CompareTo(other: Box): int\n"
        "    return Value - other.Value;\n"
        "  end CompareTo;\n"
        "end Box;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var s := new SortedSet<Box>();\n"
        "    using var b1 := new Box(30);\n"
        "    using var b2 := new Box(10);\n"
        "    using var b3 := new Box(20);\n"
        "    s.Add(b1); s.Add(b2); s.Add(b3);\n"
        "    using var arr := s.ToArray();\n"
        "    var acc := \"\";\n"
        "    for var x in arr do acc := acc + $\"{x.Value},\"; end;\n"
        "    Console.WriteLine(acc);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAotOut(src), "10,20,30,");
}
