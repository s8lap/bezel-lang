// Codegen + sema — stdlib §5 (v0.286), issue 3440: `OfType<U>()` is GONE, and
// this file measures how completely.
//
// WHY THIS FILE EXISTS.
//
// Issue 3440 reports that `OfType<U>()` "performs no runtime type test at all"
// and asks for one — a per-element test reusing `Emitter::lowerTypeTest`. That
// premise is dead. Proposal 0121 R2 was owner-accepted on 2026-08-27 and
// **v0.284 removed `OfType<U>()` from stdlib §5 entirely**; fix #3559 deleted
// the always-true-predicate lowering from `LlvmBackend_Calls3.cpp` (its `isOp`
// disjunction now carries a comment where the `OfType` term used to be) and made
// both sema arms raise an unconditional removal diagnostic. There is no operator
// left to implement a type test for. Every SIGSEGV cell in 3440's grid — the
// canonical downcast included — is a located compile error today.
//
// `tests/sema/stdlib_v0284_removals_3559_test.cpp` and
// `tests/lit/oftype_removed_v0284_3559.bzl` pin the removal on the receivers
// #3559 shipped with: `Array`, `OwningArray`, `Range`, and the lazy arm. This
// file pins the receivers they do NOT cover, and the two shapes where the
// removal measurably does not hold.
//
// That second half is the point. A removal is only as good as its narrowest
// gate, and this one had two holes, both filed and both now fixed and
// pinned here as REJECT rows:
//
//   * issue 3710 — `OfType<U>(x)`: BOTH sema arms gated on `ce.args.empty()`,
//     while the §5 name exemption in `kBuiltinCollectionLinqOperatorNames` is
//     name-only. So a WITHDRAWN member given any argument was waved past the
//     "no member" reject AND skipped by the removal arm: rc=0, linked, ran.
//     FIXED in the commit that added this row (both arms dropped the
//     `ce.args.empty()` guard — a withdrawn member is a removal diagnostic
//     at ANY arity); the row is a reject row rather than deleted, so a
//     re-introduction of the guard shows up as a regression, not silence.
//   * issue 3711/3730 — a `SortedSet<T>` receiver: the whole §5 surface (and
//     `ToArray`) lowered to a zero-initialized value there (the removed
//     `OfType` compiled for the same reason `Where` returned nothing),
//     because `isArrLike` (`ExprTypecheck_Calls.cpp`) omitted `SortedSet`
//     from its head list entirely — the SEMA half, issue 3730. FIXED by
//     #3730: `isArrLike` now admits `SortedSet`, so the whole §5 surface —
//     `OfType` included — reaches the SAME removal diagnostic every other
//     receiver family gets. The row below, formerly a KNOWN-GAP pin on the
//     `sortedToArray=0` symptom, is FLIPPED to the fixed value
//     (`sortedToArray=3`) rather than deleted, per the file's own "flip it"
//     convention; a new REJECT row (`reject_sortedset_oftype_receiver`)
//     directly pins that `s.OfType<int>()` on a `SortedSet<int>` now takes
//     the removal diagnostic, closing the loop for THIS file's actual
//     subject.
//
// AXIS GRID (receiver kind x expected outcome). Every reject row below is a
// cell issue 3440's original grid either miscompiled or never contained, and
// every one of them PASSES today — they are guards against the removal being
// partially undone, not pins on a fix in this commit (except the 3710 and
// 3730 rows, each of which WAS a fix at the revision that added/flipped it).
//
//   receiver family              row                              today
//   ---------------------------  -------------------------------  ----------
//   Stack<T>                     reject_stack_receiver            REJECT
//   Queue<T>                     reject_queue_receiver            REJECT
//   LinkedList<T>                reject_linkedlist_receiver       REJECT
//   OwningLinkedList<T>          reject_owning_linkedlist_recv    REJECT
//   OwningSet<T>                 reject_owningset_receiver        REJECT
//   SortedDictionary<K,V>        reject_sorteddictionary_recv     REJECT
//   IEnumerable<T> PARAMETER     reject_ienumerable_param_recv    REJECT
//   IGrouping<K,T>               reject_igrouping_receiver        REJECT
//   user class with own OfType   accept_user_declared_oftype      RUNS -> 42
//   Array<T>, one argument       reject_3710_oftype_with_argument REJECT
//   SortedSet<T>, direct OfType  reject_sortedset_oftype_receiver REJECT
//   SortedSet<T>, ToArray witness fixed_3730_sortedset_...        "...=3"
//
// The `Bag` row is the over-rejection control that matters most: `OfType` is an
// ordinary identifier, and a user's own method of that name must be unaffected
// by a stdlib removal. A removal implemented by short-circuiting on the NAME
// rather than on the receiver would break it, and nothing else here would
// notice.
//
// Citations verified verbatim against docs/ at v0.286: stdlib §5
// `System.Collections.Linq` declares no `OfType` row (`grep -n OfType
// docs/Bezel_Standard_Library.md` is empty); §4.18 `SortedSet<T>` declares
// `public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>`
// and stdlib line 1467 declares `public interface IReadOnlyCollection<out T>:
// IEnumerable<T>`, which is why §5 applies to a `SortedSet` at all and why 3711
// is a defect rather than a non-member.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A hierarchy with a class element, an interface, and a user-declared `OfType`.
// No C++ line comments live inside this literal — one would comment out the
// rest of the Bezel program and every row would pass for the wrong reason.
constexpr const char* kTypes = R"BZL(module M;
import System;
public interface IBarker
  method Bark(): int;
end IBarker;
public class Animal
  public Tag: int := 1;
end Animal;
public class Dog: Animal, IBarker
  public DogOnly: int := 77;
  public method Bark(): int return 7; end Bark;
end Dog;
public class Bag
  public method OfType(n: int): int return n * 2; end OfType;
end Bag;
)BZL";

std::string prog(const std::string& mainBody) {
    return std::string(kTypes) +
           "class C\n"
           "  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// True iff some ERROR diagnostic is the v0.284 removal diagnostic, naming both
// types so the reader can see which call was refused. Matches the wording
// `ExprTC::ofTypeRemovedMessage` builds (one spelling for both arms, fix #3559)
// rather than merely asserting `!ran` — a rejection for some unrelated reason
// would otherwise read as this removal holding.
bool removedOfType(const JitResult& r, const std::string& u,
                   const std::string& t) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const std::string& m = d.message;
        if (m.find("`OfType<" + u + ">()` on a sequence of `" + t + "`") ==
            std::string::npos)
            continue;
        if (m.find("was REMOVED from the standard library at v0.284 "
                   "(stdlib §5)") == std::string::npos)
            continue;
        if (m.find("the operator is gone, not misspelled (issue 3559)") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

// The interim issue-3440 "not yet implemented" wording must be gone everywhere:
// one spelling, not two. A receiver family still carrying it would mean #3559
// reached some arms and not others.
bool carriesInterim3440Wording(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is not yet implemented (stdlib §5 / issue 3440)") !=
            std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// REJECT — receiver families neither the #3559 gtest file nor its lit file
// covers. Each currently PASSES; each would go red if the removal were undone
// for that family alone.
// ===========================================================================

BEZEL_TEST(oftype_removal_completeness_3440, reject_stack_receiver) {
    auto r = runAot(prog("    using var s := new Stack<int>();\n"
                         "    s.Push(1);\n"
                         "    using var b := s.OfType<int>().ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

BEZEL_TEST(oftype_removal_completeness_3440, reject_queue_receiver) {
    auto r = runAot(prog("    using var s := new Queue<int>();\n"
                         "    s.Enqueue(1);\n"
                         "    using var b := s.OfType<int>().ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

BEZEL_TEST(oftype_removal_completeness_3440, reject_linkedlist_receiver) {
    auto r = runAot(prog("    using var s := new LinkedList<int>();\n"
                         "    s.Add(1);\n"
                         "    using var b := s.OfType<int>().ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

// A CLASS element on an owning collection — the element category the original
// 3440 grid crashed on, now on a family #3559 did not test.
BEZEL_TEST(oftype_removal_completeness_3440,
           reject_owning_linkedlist_class_element_receiver) {
    auto r = runAot(prog("    using var s := new OwningLinkedList<Dog>();\n"
                         "    s.Add(new Dog());\n"
                         "    var n := 0;\n"
                         "    for var d in s.OfType<Dog>() do n := n + 1; end;\n"
                         "    return n;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "Dog", "Dog"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

BEZEL_TEST(oftype_removal_completeness_3440,
           reject_owningset_class_element_receiver) {
    auto r = runAot(prog("    using var s := new OwningSet<Dog>();\n"
                         "    s.Add(new Dog());\n"
                         "    var n := 0;\n"
                         "    for var d in s.OfType<Dog>() do n := n + 1; end;\n"
                         "    return n;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "Dog", "Dog"));
}

// A map receiver: the element is a `DictionaryEntry<K, V>`, which the diagnostic
// renders as the unnamed pair `<int, int>`. Pinned with that exact spelling so a
// change to `typeNameForDiag` (issue 3456) is visible rather than silent.
BEZEL_TEST(oftype_removal_completeness_3440,
           reject_sorteddictionary_receiver) {
    auto r = runAot(prog("    using var s := new SortedDictionary<int,int>();\n"
                         "    s.Add(1, 2);\n"
                         "    using var b := s.OfType<int>().ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "<int, int>"));
}

// An `IEnumerable<T>`-typed PARAMETER receiver. This shape is neither a concrete
// collection nor a lazy chain, and the interim comment on issue 3440 records it
// as one of two shapes that issue's own grid never contained.
BEZEL_TEST(oftype_removal_completeness_3440,
           reject_ienumerable_parameter_receiver) {
    auto r = runAot(std::string(kTypes) +
                    "class C\n"
                    "  public static method F(xs: IEnumerable<int>): int\n"
                    "    using var b := xs.OfType<int>().ToArray();\n"
                    "    return b.Count;\n"
                    "  end F;\n"
                    "  public static method Main(): int\n"
                    "    using var a := new Array<int>();\n"
                    "    a.Add(1);\n"
                    "    return F(a);\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

// An `IGrouping<TKey, T>` receiver reaches the lazy arm through a third route
// (the synthetic grouping element type), not through a `Where` chain.
BEZEL_TEST(oftype_removal_completeness_3440, reject_igrouping_receiver) {
    auto r = runAot(
        prog("    using var a := new Array<int>();\n"
             "    a.Add(1);\n"
             "    var n := 0;\n"
             "    for var g in a.GroupBy(lambda(x: int) => x) do\n"
             "      for var e in g.OfType<int>() do n := n + e; end;\n"
             "    end;\n"
             "    return n;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

// issue 3710 (fixed in this commit). `OfType<U>(x)` — a member stdlib §5 no
// longer declares, given an argument — used to compile rc=0 and run, because
// the §5 name exemption is arity-blind while both removal arms gated on
// `ce.args.empty()`. The `direct=3` half is the control: the source really
// does hold three elements, so a silent `withArg=0` was a wrong ANSWER and
// not an empty input. fix #3710 drops the `ce.args.empty()` guard from both
// arms (ExprTypecheck_Calls.cpp ~:4733 and ~:6265), so a withdrawn member is
// a removal diagnostic at ANY arity. This row was the known-gap pin before
// the fix; it is kept (flipped, not deleted) so a re-introduced arity guard
// shows up as a regression here rather than silently reopening the gap.
BEZEL_TEST(oftype_removal_completeness_3440,
           reject_3710_oftype_with_argument_now_rejects) {
    auto r = runAot(prog("    using var a := new Array<int>();\n"
                         "    a.Add(1); a.Add(2); a.Add(3);\n"
                         "    Console.WriteLine($\"direct={a.Count}\");\n"
                         "    using var b := a.OfType<int>(0).ToArray();\n"
                         "    var c := b.Count;\n"
                         "    Console.WriteLine($\"withArg={c}\");\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

// ===========================================================================
// ACCEPT-CONTROL — the removal is about a stdlib member, not about the
// identifier. A user's own `OfType` method must be untouched.
// ===========================================================================

BEZEL_TEST(oftype_removal_completeness_3440,
           accept_user_declared_oftype_method_still_runs) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    using var g := new Bag();\n"
                                   "    var v := g.OfType(21);\n"
                                   "    Console.WriteLine($\"bag={v}\");\n"
                                   "    return 0;")),
                    "bag=42");
}

// ===========================================================================
// FIXED (was a KNOWN-GAP row) — fix #3730: `isArrLike`
// (ExprTypecheck_Calls.cpp) admits `SortedSet<T>` now, so the whole stdlib
// §5 surface over a direct `SortedSet<T>` receiver types (and, via #3711's
// codegen fix, lowers correctly) instead of degrading to a zero-initialized
// value. This row is the SAME program the KNOWN-GAP row previously pinned as
// broken — flipped to the fixed value per the file's own "flip it" rule
// rather than deleted, so a regression of #3730 shows up here as a returned
// row rather than silence. Two controls in the SAME program make this
// impossible to pass vacuously: the `Set<int>` line is the identical shape
// three statements earlier and is (and was always) correct, and `for-in`
// over the very same `SortedSet` sums 3+1+2 = 6, proving the set is
// populated independent of the §5 path under test.
// ===========================================================================
BEZEL_TEST(oftype_removal_completeness_3440,
           fixed_3730_sortedset_linq_path_now_matches_forin) {
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
                       "    Console.WriteLine($\"forin={n}\");\n"
                       "    return 0;")),
        "setToArray=3\nsortedToArray=3\nforin=6");
}

// ===========================================================================
// This file's actual subject, directly: `OfType<U>()` on a DIRECT
// `SortedSet<int>` receiver now reaches the SAME v0.284 removal diagnostic
// every other receiver family in this file gets — before #3730, `isArrLike`
// excluding `SortedSet` meant this call never reached the `OfType` arm at
// all (it degraded via #2434 instead, per the row above, pre-fix). This is
// the direct proof the removal gate is complete for `SortedSet`; the row
// above is a witness via `ToArray` for the surrounding `isArrLike` fix.
// ===========================================================================
BEZEL_TEST(oftype_removal_completeness_3440,
           reject_sortedset_oftype_receiver) {
    auto r = runAot(prog("    using var s := new SortedSet<int>();\n"
                          "    s.Add(3); s.Add(1); s.Add(2);\n"
                          "    var v := s.OfType<int>();\n"
                          "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}
