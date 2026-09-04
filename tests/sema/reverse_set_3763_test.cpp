// Sema + codegen — stdlib §5 / §4.6 / §4.7 / §4.10 / §4.18 (v0.287), issue
// 3763: stdlib §5's LAZY `Reverse(): IEnumerable<T>` had NO sema typing arm on
// a DIRECT builtin-collection receiver. `typeLinqOperator`
// (ExprTypecheck_Calls.cpp) — the fallback that types every lazy §5 operator
// over a collection receiver — never mentioned `Reverse`, so the call acquired
// no `exprType` and the two receiver families on which §5's operator is the
// ONLY declared meaning failed, differently and all wrongly:
//
//   * `Set<int>` / `LinkedList<int>`: the call was left untyped and the NEXT
//     line degraded through #2434 (`lowerLocalDecl`'s zero-initialized slot),
//     so `Console.WriteLine(...)` emitted nothing at all — a QUIET wrong
//     answer, and LOUD (`#2457 "produced no value"`) or QUIET depending only
//     on whether the call was split across two locals, the inconsistency §6c
//     exists to close.
//   * `SortedSet<int>`: carried a dedicated 3763-citing LOUD reject
//     (`ExprTypecheck_Calls.cpp`, added by #3735's lane) that kept the failure
//     located until this fix landed.
//   * a CLASS-element `Set<T>`: worst of the three. With no `exprType` on the
//     call, codegen's `isOp` gate in `tryLowerLazyLinqCall` refused `Reverse`
//     on a non-lazy receiver, so it fell through to
//     `tryLowerCollectionInstanceCall`'s IN-PLACE `bz_array_*` reverse and
//     silently MUTATED the receiver — measured, pre-fix, at exit 0 with no
//     diagnostic: a `Set<W>` holding W(1), W(2), W(3) enumerated `1,2,3`
//     before `s.Reverse();` and `3,2,1` after, where §5 promises a fresh lazy
//     sequence and leaves the source untouched.
//
// THE FIX has two halves and needs both (#3735 measured the codegen half INERT
// on its own — with no `exprType` on the call nothing reaches the lowering):
//   1. sema: a `Reverse` arm in `typeLinqOperator`, gated on
//      `common::isLazyReverseDirectReceiverHead`;
//   2. codegen: the `revRecv` term in `LlvmBackend_Calls3.cpp`'s `isOp`
//      `Reverse` gate, reading the SAME predicate.
// The SortedSet reject is deleted in the same commit (lane rules §7).
//
// THE §6b EXCLUSION DIFF is the second half of this file (Category B). The
// admission is receiver-gated, not a name predicate, because `Reverse` is the
// one §5 operator whose applicability is RECEIVER-dependent: spec §26.5 rule 1
// gives the in-place instance member the name wherever one is declared. Every
// head the fix must NOT move is pinned below with its pre-fix behaviour, so a
// later widening of the predicate cannot land silently.
//
// Citations verified verbatim against docs/ at v0.287:
//   stdlib §5 (line 3544): "public method Reverse(): IEnumerable<T;"
//   stdlib §5 naming note (line 3608, v0.195): "on an `Array<T>` receiver,
//     instance resolution commits to the in-place `Reverse()` before
//     extensions are consulted (spec §26.5 rule 1 …); the lazy operator
//     applies only where the receiver's static type has no instance
//     `Reverse`."
//   stdlib §4.4 `Array<T>` (line 1583): "public method Reverse();  // in place"
//   stdlib §4.5 `OwningArray<T>` (line 1678): "public method Reverse();"
//   — those two, plus `Span<T>`/`ReadOnlySpan<T>` and `ArraySpan<T>` (§4.15),
//   are the ONLY types in the document that declare an instance `Reverse()`.
//   §4.10 `Set<T>`/`OwningSet<T>`, §4.18 `SortedSet<T>`, §4.6/§4.7
//   `LinkedList<T>`/`OwningLinkedList<T>` and §4.11 `Stack`/`Queue` declare
//   none.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M;\n"
           "import System;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           mainBody +
           "\n    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Joins a collection's elements into "a,b,c," so an ORDER assertion is a
// single string compare. Kept as source text (not a Bezel helper method) so
// each row stays a self-contained program.
std::string joinLoop(const std::string& src, const std::string& accName) {
    return "    var " + accName + " := \"\";\n"
           "    for var jx in " + src + " do\n"
           "      " + accName + " := " + accName + " + jx.ToString() + \",\";\n"
           "    end;\n";
}

// ===========================================================================
// Category A — the fix itself. Every row runs the program and asserts a VALUE
// (`runAotOut`), never "it compiled": the pre-fix failure on a `Set` receiver
// was a silently zero-degraded slot that still exits 0.
// ===========================================================================

// The issue's own repro, split-local form — the shape that degraded QUIETLY
// (#2434) pre-fix. `s.Reverse()` must now type as a lazy IEnumerable<int>,
// and `FirstOrNull()` on it must yield the LAST element of the source's own
// enumeration order.
BEZEL_TEST(reverse_set_3763, set_reverse_split_local_firstornull_is_last) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new Set<int>();\n"
            "    s.Add(1); s.Add(2); s.Add(3);\n"
            "    var r := s.Reverse();\n"
            "    var first := r.FirstOrNull();\n"
            "    Console.WriteLine((first ?? 0).ToString());")),
        "3");
}

// The CHAINED form of the same call. Pre-fix the two forms disagreed — this
// one raised the LOUD #2457 "`Reverse` on built-in type `Set` produced no
// value" while the split form above degraded quietly — which is precisely the
// §6c loud/quiet inconsistency the fix closes. Both must now agree on 3.
BEZEL_TEST(reverse_set_3763, set_reverse_chained_form_agrees_with_split) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new Set<int>();\n"
            "    s.Add(1); s.Add(2); s.Add(3);\n"
            "    var first := s.Reverse().FirstOrNull();\n"
            "    Console.WriteLine((first ?? 0).ToString());")),
        "3");
}

// ORDER, not just arity: stdlib §5's `Reverse()` yields the source sequence
// backwards. A `Set<int>` enumerates in insertion order (measured: `{30,10,
// 20}` enumerates `30,10,20` through both `ToArray()` and `for`-in), so the
// reversed sequence is `20,10,30`. Asserting the concrete string rather than
// `Count` is what distinguishes a real reversal from any materialization.
BEZEL_TEST(reverse_set_3763, set_reverse_yields_source_order_backwards) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new Set<int>();\n"
            "    s.Add(30); s.Add(10); s.Add(20);\n"
            "    using var rv := s.Reverse().ToArray();\n" +
            joinLoop("rv", "acc") +
            "    Console.WriteLine(acc);")),
        "20,10,30,");
}

// `SortedSet<T>` (§4.18) iterates in ascending order, so its §5 `Reverse()` is
// the descending sequence. This row ALSO pins that #3735's dedicated
// 3763-citing reject is gone: pre-fix this program did not run at all.
BEZEL_TEST(reverse_set_3763, sortedset_reverse_yields_descending) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new SortedSet<int>();\n"
            "    s.Add(30); s.Add(10); s.Add(20);\n"
            "    using var rv := s.Reverse().ToArray();\n" +
            joinLoop("rv", "acc") +
            "    Console.WriteLine(acc);")),
        "30,20,10,");
}

// The reject's own text must be GONE, not merely unreached — a diagnostic
// reported once is not undone by a later correct type.
BEZEL_TEST(reverse_set_3763, sortedset_reverse_reject_text_is_gone) {
    auto r = runAot(prog(
        "    using var s := new SortedSet<int>();\n"
        "    s.Add(30); s.Add(10); s.Add(20);\n"
        "    using var rv := s.Reverse().ToArray();\n"
        "    Console.WriteLine(rv.Count.ToString());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "issue 3763"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "no typing on a direct"));
}

// `LinkedList<T>` (§4.6) is `bz_llist_t*`-backed, not array-backed, and
// declares no `Reverse()` either. Pre-fix it took the same #2434 quiet
// degrade as `Set`.
BEZEL_TEST(reverse_set_3763, linkedlist_reverse_yields_backwards) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var l := new LinkedList<int>();\n"
            "    l.AddLast(11); l.AddLast(22); l.AddLast(33);\n"
            "    using var rv := l.Reverse().ToArray();\n" +
            joinLoop("rv", "acc") +
            "    Console.WriteLine(acc);")),
        "33,22,11,");
}

// THE SOUNDNESS HALF. Pre-fix, a class-element `Set<T>` reached the in-place
// `bz_array_*` mutator and `s.Reverse();` as a bare statement REVERSED THE
// RECEIVER (measured `1,2,3` -> `3,2,1`, exit 0, no diagnostic). §5's operator
// is non-destructive: the source must read identically before and after, and
// the sequence the operator returns must be the reversal.
BEZEL_TEST(reverse_set_3763, class_element_set_reverse_does_not_mutate_source) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M;\n"
            "import System;\n"
            "class W\n"
            "  public property V: int;\n"
            "  public constructor(x: int)\n"
            "    this.V := x;\n"
            "  end constructor;\n"
            "end W;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var w1 := new W(1);\n"
            "    using var w2 := new W(2);\n"
            "    using var w3 := new W(3);\n"
            "    using var s := new Set<W>();\n"
            "    s.Add(w1); s.Add(w2); s.Add(w3);\n"
            "    using var rv := s.Reverse().ToArray();\n"
            "    var back := \"\";\n"
            "    for var e in rv do\n"
            "      back := back + e.V.ToString();\n"
            "    end;\n"
            "    var src := \"\";\n"
            "    for var e2 in s do\n"
            "      src := src + e2.V.ToString();\n"
            "    end;\n"
            "    Console.WriteLine($\"{back}/{src}\");\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "321/123");
}

// AMPLIFIED (lane rules §3: a one-shot allocation row passes on both sides
// because the allocation is still reachable at exit). 500 iterations, each
// building a fresh `Set`, its forward snapshot and its reversed snapshot, and
// checking the reversal PROPERTY against the source's own order — an oracle
// that does not assume any particular hash/insertion order. Every iteration
// must satisfy it, so the printed count is exactly 500; under the gate's ASan
// oracle the same row is the leak/UAF check for 1500 lazy pipelines.
BEZEL_TEST(reverse_set_3763, set_reverse_property_holds_over_500_iterations) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var okCount := 0;\n"
            "    for var i in 1..500 do\n"
            "      using var s := new Set<int>();\n"
            "      s.Add(i); s.Add(i + 1); s.Add(i + 2);\n"
            "      using var fwd := s.ToArray();\n"
            "      using var rev := s.Reverse().ToArray();\n"
            "      if rev.Count = fwd.Count and rev[0] = fwd[fwd.Count - 1] "
            "and rev[fwd.Count - 1] = fwd[0] and rev[1] = fwd[1] then\n"
            "        okCount := okCount + 1;\n"
            "      end if;\n"
            "    end;\n"
            "    Console.WriteLine(okCount.ToString());")),
        "500");
}

// ===========================================================================
// Category B — the §6b exclusion diff. The admission is a RECEIVER gate, and
// these rows are what it must keep excluding. Each pins the behaviour that
// was correct (or deliberately accepted) BEFORE the fix, so a later widening
// of `common::isLazyReverseDirectReceiverHead` cannot land unnoticed.
// ===========================================================================

// `Array<T>` (§4.4) DECLARES an in-place `Reverse()`. Per spec §26.5 rule 1
// the instance member wins by name and §5's operator is unreachable here —
// the fix must not retarget it to a lazy sequence.
BEZEL_TEST(reverse_set_3763, array_reverse_still_mutates_in_place) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Array<int>();\n"
            "    a.Add(1); a.Add(2); a.Add(3);\n"
            "    a.Reverse();\n"
            "    Console.WriteLine($\"{a[0]},{a[1]},{a[2]}\");")),
        "3,2,1");
}

// …and chaining onto its unit result stays the #466 LOUD reject, with its
// §26.5 wording intact.
BEZEL_TEST(reverse_set_3763, array_reverse_chain_still_rejected_466) {
    auto r = runAot(prog(
        "    using var a := new Array<int>();\n"
        "    a.Add(1); a.Add(2); a.Add(3);\n"
        "    using var rv := a.Reverse().ToArray();\n"
        "    Console.WriteLine(rv.Count.ToString());"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "returns nothing (it modifies the collection in place"));
}

// `OwningArray<T>` (§4.5) declares the same in-place `Reverse()`. Uses a
// small class element because OwningArray rejects primitives and `string`
// per §4.2 / §14.4 (#1327).
BEZEL_TEST(reverse_set_3763, owningarray_reverse_still_mutates_in_place) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M;\n"
            "import System;\n"
            "class Box public v: int; public constructor(x: int) v := x; end constructor; end Box;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var a := new OwningArray<Box>();\n"
            "    a.Add(new Box(1)); a.Add(new Box(2)); a.Add(new Box(3));\n"
            "    a.Reverse();\n"
            "    Console.WriteLine($\"{a[0].v},{a[1].v},{a[2].v}\");\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "3,2,1");
}

// `Stack<T>` (§4.11) declares NO `Reverse()`, so stdlib §5's naming note says
// the lazy operator SHOULD apply — but this compiler structurally dispatches
// it to the in-place `bz_array_*` mutator, and #2457 slice 2 measured that
// mutation VALUE-CORRECT and accepted it deliberately
// (`stack_queue_unresolved_member_2457_test.cpp`'s
// `legit_reverse_alias_stack_value_correct`). Retargeting it would change the
// meaning of WORKING code, not repair a broken path, so #3763's predicate
// excludes the Stack/Queue family and this row pins that it is unmoved:
// Push 1,2,3 then Reverse() then Pop() yields the first-pushed element.
BEZEL_TEST(reverse_set_3763, stack_reverse_still_in_place_unmoved) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new Stack<int>();\n"
            "    s.Push(1); s.Push(2); s.Push(3);\n"
            "    s.Reverse();\n"
            "    Console.WriteLine(s.Pop().ToString());")),
        "1");
}

// The Queue twin of the row above.
BEZEL_TEST(reverse_set_3763, queue_reverse_still_in_place_unmoved) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var q := new Queue<int>();\n"
            "    q.Enqueue(1); q.Enqueue(2); q.Enqueue(3);\n"
            "    q.Reverse();\n"
            "    Console.WriteLine(q.Dequeue().ToString());")),
        "3");
}

// `OwningBuffer<T>` (§4.14) is `bz_array_t*`-backed and reaches the same
// in-place mutator (measured pre-fix: `[1,2,3]` then `Reverse()` then `b[0]`
// is 3). Excluded for the same reason as the Stack family.
BEZEL_TEST(reverse_set_3763, owningbuffer_reverse_still_in_place_unmoved) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var b := new OwningBuffer<int>();\n"
            "    b.Add(1); b.Add(2); b.Add(3);\n"
            "    b.Reverse();\n"
            "    Console.WriteLine(b[0].ToString());")),
        "3");
}

// The lazy-receiver and Range forms `isOp` already admitted must be
// untouched: the fix ADDS a term to that gate's disjunction, so a regression
// here would mean the edit changed the existing terms rather than extending
// them.
BEZEL_TEST(reverse_set_3763, lazy_and_range_receivers_unaffected) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Array<int>();\n"
            "    a.Add(1); a.Add(2); a.Add(3);\n"
            "    using var w := a.Where(lambda(x) => x > 0).Reverse().ToArray();\n" +
            joinLoop("w", "acc") +
            "    Console.WriteLine(acc + (1..5).Reverse().First().ToString());")),
        "3,2,1,5");
}

}  // namespace
