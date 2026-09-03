// Codegen — stdlib §5.1 (v0.286), issue 3764: `MinOrNull()`/`MaxOrNull()`
// missing from `isArrTerminal` in `Emitter::tryLowerCollectionInstanceCall`
// (src/codegen/LlvmBackend_Calls.cpp).
//
// WHAT WAS BROKEN. `isArrTerminal` is the list of stdlib §5 terminal
// operators that get MATERIALIZED (via `lowerToLazyEnumerator` +
// `bz_lazy_to_array`) before running the concrete-array terminal lowerings,
// for every non-array-backed source family: a lazy `IEnumerable<T>` chain,
// `LinkedList<T>`, `Range<T>`, a vtable-backed `IEnumerable<T>`, `string`,
// and — since fix #3735 (b43c940) — the dict-backed `Set`/`OwningSet`/
// `SortedSet` family. `MinOrNull`/`MaxOrNull` were absent from that list,
// even though the array arm that implements them already exists (fix
// #2783, ~line 3734) and reads its receiver only via
// `bz_array_len`/`bz_array_ptr_at` — the exact same shape every other
// `isArrTerminal` member uses. So a NON-array receiver's `.MinOrNull()` /
// `.MaxOrNull()` fell all the way through `tryLowerCollectionInstanceCall`
// (none of `isArr`/`isDict`/`isStrBld`/`lazyTerminal` were true) and
// degraded via #2434 (a zero-initialized local slot) or #1386 (inside a
// string interpolation) — the issue's own repro, reproduced verbatim below.
//
// THE FIX. One term added to the existing `isArrTerminal` disjunction — no
// new per-operator arm, exactly the shape fixes #450/#775/#995/#1220/#3735
// already used for their own source family. `MinOrNull`/`MaxOrNull` compose
// unchanged over the materialized snapshot because the array arm never
// assumed anything about how `recv` got to be a `bz_array_t*`.
//
// §6b EXCLUSION DIFF. Before this fix, `isArrTerminal` admitted every §5
// terminal EXCEPT `MinOrNull`/`MaxOrNull` for a non-array receiver; those two
// names still worked on a concrete `Array<T>` receiver (fix #2783, untouched
// here). After this fix, both names are admitted for every source family
// already on the `lazyTerminal` disjunction: lazy chain, `LinkedList`,
// `Range`, vtable-`IEnumerable`, and the dict-backed Set family (a bonus of
// sharing the one list #3735 hoisted `isSetName`/`setRecvTerm` into — no new
// classification was added for MinOrNull/MaxOrNull specifically). NOTHING
// ELSE moved: every other name in `isArrTerminal` is byte-identical to
// before, and `dictSetArmClaims` (Contains/no-arg Count/SortedSet no-arg
// First-Last) is untouched, so those three keep their dedicated `bz_dict_*`
// arms exactly as `#3735` left them — `other_operators_still_route_the_same`
// below pins that.
//
// `string` WAS DELIBERATELY EXCLUDED HERE, AND IS NO LONGER (fix #3786).
// At the time this file was written, `"abc".MinOrNull()` drew "string member
// `.MinOrNull` is not yet supported in codegen" from a SEPARATE, sema-level
// closed-surface gate (`common::isStringLinqOperatorMemberName`,
// src/common/BuiltinRegistry.cpp's `kStringLinqOperatorMemberNames`, then 37
// names) that runs in `ExprTypecheck_Calls.cpp` BEFORE codegen is ever
// reached — this fix (which only touched codegen) could not reach it, and
// this file did not claim to. That was the residual half of issue #3444
// ("isLinqOp name list has drifted from stdlib §5 — MinOrNull/MaxOrNull work
// on Array<T> but are refused on a string receiver"), which fix #3589's
// consolidation deliberately left untouched per its own commit body ("its
// narrower string-receiver MinOrNull/MaxOrNull/ElementAtOrNull widening
// question is untouched, per its own explicit warning against adding names
// without exercising the lowering end-to-end"). Fix #3786 completed that
// proof — the string materialize path (`isStringRecvTerm`, already part of
// the `lazyTerminal` disjunction this fix widened) composes with the same
// #2783 array arm exactly as the other five families above — and added
// `MinOrNull`/`MaxOrNull` to `kStringLinqOperatorMemberNames` (39 names).
// See `minornull_maxornull_string_receiver_3786_test.cpp` for the
// string-receiver value rows; this file's own scope stays the five
// originally-fixed non-array families plus the `string`-still-excluded
// history above, kept for the record.
//
// FAIL-WITHOUT / PASS-WITH (lane rules §R12-H): with `isArrTerminal` reverted
// to its pre-fix form (a scratch stash of just this hunk, never `git
// checkout` over uncommitted work), every non-`*_still_route_the_same`,
// non-string row below either aborts the compile with the #2434/#1386
// internal-degrade diagnostic or (Set/SortedSet, since #3735's dict arm
// blanket `return nullptr`s for anything not in its claim list) rejects with
// "no dispatch arm emitted any code for this call". With the fix, every row
// compiles and runs to the value asserted. See the commit body for the exact
// filter counts.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// `Box: IComparable<Box>` — the class-element receiver, shared by the lazy-
// chain and SortedSet class-element rows. Mirrors the Box used in
// tests/codegen/set_family_eager_terminals_3735_test.cpp.
const char* PRELUDE =
    "module M;\n"
    "import System;\n"
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

}  // namespace

// ===========================================================================
// The issue's OWN repro (GH #3764): an Array<int> receiver already works
// (fix #2783); a lazy chain built off it degraded silently via #2434 — a
// zero-initialized slot. ⚠ The issue's own `m2 = null` null-check text is
// NOT a sufficient oracle here: measured, the pre-fix zero-initialized `T?`
// slot happens to compare as "not null" too, so a row asserting only
// "False\nFalse" passes on BOTH sides of this fix (confirmed mechanically —
// see the commit body) and would silently NOT pin the bug. The THIRD line
// (the unwrapped VALUE, 20 — the min of {30,20} once >15 filters out 10) is
// what actually differs: the pre-fix degrade prints a wrong/zero value,
// where the array-receiver control lines are unaffected either way.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764, issue_repro_verbatim) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M;\n"
            "import System;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    var a: Array<int> := [30, 10, 20];\n"
            "    var m := a.MinOrNull();\n"
            "    Console.WriteLine(m = null);\n"
            "    var lz := a.Where(lambda(x: int) => x > 15);\n"
            "    var m2 := lz.MinOrNull();\n"
            "    Console.WriteLine(m2 = null);\n"
            "    Console.WriteLine(m2!);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "False\nFalse\n20");
}

// ===========================================================================
// Lazy IEnumerable<T> chain (fix #450's family). A bound lazy var is single-
// consumption (#1252/§29.11), so MinOrNull and MaxOrNull each get their OWN
// `.Where(...)` chain — reusing one bound var for both would (correctly)
// throw InvalidOperationException on the second call, an unrelated invariant
// this file does not exercise.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           lazy_chain_int_nonempty) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var arr: Array<int> := [30, 10, 20];\n"
            "    var mn: int? := arr.Where(lambda(x: int) => x > 5).MinOrNull();\n"
            "    var mx: int? := arr.Where(lambda(x: int) => x > 5).MaxOrNull();\n"
            "    Console.WriteLine($\"{mn!},{mx!}\");")),
        "10,30");
}

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           lazy_chain_int_empty_is_null) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var arr: Array<int> := [30, 10, 20];\n"
            "    var mn := arr.Where(lambda(x: int) => x > 100).MinOrNull();\n"
            "    Console.WriteLine(mn = null);")),
        "True");
}

// The class-element (IComparable<T>) row over a lazy chain — walks the same
// `CompareTo` dispatch the interface-element #2783 array test exercises, now
// over a MATERIALIZED snapshot rather than a direct array. Values {3,1,2}:
// MinOrNull.Value = 1, MaxOrNull.Value = 3.
BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           lazy_chain_class_element_icomparable) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Box(3);\n"
            "    using var b := new Box(1);\n"
            "    using var c := new Box(2);\n"
            "    var arr: Array<Box> := [a, b, c];\n"
            "    var mn: Box? := arr.Where(lambda(x: Box) => true).MinOrNull();\n"
            "    var mx: Box? := arr.Where(lambda(x: Box) => true).MaxOrNull();\n"
            "    Console.WriteLine($\"{mn!.Value},{mx!.Value}\");")),
        "1,3");
}

// ===========================================================================
// LinkedList<T> (fix #450's family — NOT `lazyRecv`, so a bound var is
// reusable across both calls: LlvmBackend_Calls.cpp's own comment records
// that only a genuine lazy receiver's bound slot is nulled after materialize).
// ===========================================================================

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           linkedlist_int_nonempty) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var ll := new LinkedList<int>();\n"
            "    ll.AddLast(30); ll.AddLast(10); ll.AddLast(20);\n"
            "    var mn: int? := ll.MinOrNull();\n"
            "    var mx: int? := ll.MaxOrNull();\n"
            "    Console.WriteLine($\"{mn!},{mx!}\");")),
        "10,30");
}

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           linkedlist_int_empty_is_null) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var ll := new LinkedList<int>();\n"
            "    var mn := ll.MinOrNull();\n"
            "    Console.WriteLine(mn = null);")),
        "True");
}

// ===========================================================================
// Range<int> (fix #775's family), receiver-position syntax `(a..b)`.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764, range_int) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var mn: int? := (1..5).MinOrNull();\n"
            "    var mx: int? := (1..5).MaxOrNull();\n"
            "    Console.WriteLine($\"{mn!},{mx!}\");")),
        "1,5");
}

// ===========================================================================
// vtable-backed IEnumerable<T> (fix #995's family) — an interface-typed local
// bound to a yield-method result. Not `lazyRecv`, so the bound var is
// reusable across both calls (same rule as LinkedList/Range above).
// ===========================================================================

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           vtable_ienumerable_nonempty) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M;\n"
            "import System;\n"
            "import System.Collections.Generic;\n"
            "class P\n"
            "  public static method Seq(): IEnumerable<int>\n"
            "    yield return 30; yield return 10; yield return 20;\n"
            "  end Seq;\n"
            "  public static method Main(): int\n"
            "    var e: IEnumerable<int> := P.Seq();\n"
            "    var mn: int? := e.MinOrNull();\n"
            "    var mx: int? := e.MaxOrNull();\n"
            "    Console.WriteLine($\"{mn!},{mx!}\");\n"
            "    return 0;\n"
            "  end Main;\n"
            "end P;\n"),
        "10,30");
}

// ===========================================================================
// The dict-backed Set family — a BONUS of sharing `isArrTerminal` with
// #3735's `setRecvTerm`, not a separately-added classification. Before this
// fix, `Set<int>.MinOrNull()` was a LOUD reject ("no dispatch arm emitted any
// code for this call" — #3735 deliberately did NOT widen `isArrTerminal`, so
// the dict block's blanket `return nullptr` was reached), never the silent
// #2434 degrade the other families hit — issue 3764's own "Scope" section
// records this distinction. Not a regression from #3735: `isArrTerminal`
// simply did not list these two names for ANY family until now.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764, set_int_nonempty) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new Set<int>();\n"
            "    s.Add(30); s.Add(10); s.Add(20);\n"
            "    var mn: int? := s.MinOrNull();\n"
            "    var mx: int? := s.MaxOrNull();\n"
            "    Console.WriteLine($\"{mn!},{mx!}\");")),
        "10,30");
}

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           set_int_empty_is_null) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new Set<int>();\n"
            "    var mn := s.MinOrNull();\n"
            "    Console.WriteLine(mn = null);")),
        "True");
}

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764, sortedset_int) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new SortedSet<int>();\n"
            "    s.Add(30); s.Add(10); s.Add(20);\n"
            "    var mn: int? := s.MinOrNull();\n"
            "    var mx: int? := s.MaxOrNull();\n"
            "    Console.WriteLine($\"{mn!},{mx!}\");")),
        "10,30");
}

// SortedSet admits a CLASS element (#2773) and is still dict-backed there
// (unlike Set/OwningSet, whose class-element instantiation takes the ARRAY
// representation — #3735's refutation, `genericBackingType`'s `isSet`
// exclusion). Walks the same class-element CompareTo arm as the lazy-chain
// row above, this time reached via `setRecvTerm` instead of `lazyRecv`.
BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           sortedset_class_element_icomparable) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new SortedSet<Box>();\n"
            "    using var b1 := new Box(30);\n"
            "    using var b2 := new Box(10);\n"
            "    using var b3 := new Box(20);\n"
            "    s.Add(b1); s.Add(b2); s.Add(b3);\n"
            "    var mn: Box? := s.MinOrNull();\n"
            "    var mx: Box? := s.MaxOrNull();\n"
            "    Console.WriteLine($\"{mn!.Value},{mx!.Value}\");")),
        "10,30");
}

// ===========================================================================
// Object parity: no OTHER operator's classification moved. `Sum`/`Count`/
// `First`/`Contains`/no-arg `Count` (the Set dict-arm exclusions #3735 left
// in place) all still route exactly as before this fix, across the families
// above in one program.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           other_operators_still_route_the_same) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var arr: Array<int> := [30, 10, 20];\n"
            "    var lzSum := arr.Where(lambda(x: int) => x > 5).Sum();\n"
            "    using var ll := new LinkedList<int>();\n"
            "    ll.AddLast(30); ll.AddLast(10); ll.AddLast(20);\n"
            "    using var s := new Set<int>();\n"
            "    s.Add(30); s.Add(10); s.Add(20);\n"
            "    var rgSum := (1..5).Sum();\n"
            "    Console.WriteLine($\"{lzSum},{ll.Count},{s.Count()},"
            "{s.Contains(10)},{rgSum}\");")),
        "60,3,3,True,15");
}

// ===========================================================================
// LEAK ROWS — `runAotChecked` (ASan/LSan), LOOP-AMPLIFIED to 200 iterations
// per lane rules §R12-G/§3. The materialized snapshot is a fresh `bz_array_t`
// freed EXACTLY ONCE at the terminal's return (`freeLazyTemp`), before either
// call in the pair returns to the loop body, so a double-free or a leaked
// snapshot on either MinOrNull or MaxOrNull shows up here.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           lazy_chain_int_leak_free_200x) {
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      var arr: Array<int> := [30, 10, 20];\n"
        "      var mn: int? := arr.Where(lambda(x: int) => x > 5).MinOrNull();\n"
        "      var mx: int? := arr.Where(lambda(x: int) => x > 5).MaxOrNull();\n"
        "      total := total + mn! + mx!;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    // (10 + 30) * 200 = 8000.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=8000\n"));
}

BEZEL_TEST(minornull_maxornull_nonarray_terminal_3764,
           sortedset_class_element_leak_free_200x) {
    // Elements are BORROWED by the SortedSet (stdlib §28.11's borrow-slot
    // row); each `Box` is disposed exactly once by its own `using var`, never
    // by the set and never by the terminal's snapshot — mirrors
    // `sortedset_class_elements_terminals_leak_free_200x` in the #3735 file.
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var s := new SortedSet<Box>();\n"
        "      using var b1 := new Box(30);\n"
        "      using var b2 := new Box(10);\n"
        "      using var b3 := new Box(20);\n"
        "      s.Add(b1); s.Add(b2); s.Add(b3);\n"
        "      var mn: Box? := s.MinOrNull();\n"
        "      var mx: Box? := s.MaxOrNull();\n"
        "      total := total + mn!.Value + mx!.Value;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    // (10 + 30) * 200 = 8000.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=8000\n"));
}
