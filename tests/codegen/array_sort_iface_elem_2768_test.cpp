// Spec-derived regression tests for #2768 (v0.240): `Array<T>.Sort()` /
// `.Sorted()` when T is an INTERFACE type.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §4.4 declares the comparerless forms only
// inside
//     public extension Array<T> where T: IComparable<T>
//       public method Sort();                    // in place
//       public method Sorted(): owned Array<T>;  // fresh sorted copy
//     end;
// §4.4's sorting contract names `CompareTo` as the ordering authority — "A
// comparer (or `CompareTo`) must induce a **total order**" — and stdlib §2.8
// fixes its meaning: "`a.CompareTo(b)` returns a negative number if `a < b`,
// zero if equal, a positive number if `a > b`."  §4.4 also fixes the naming
// convention the Sorted() case below asserts: "the past-participle form
// (`Reversed`, `Sorted`) leaves the receiver unchanged and returns a fresh
// `owned Array<T>` with the elements in the new order."
//
// spec §14.2's interface-conformance rule 3 admits an interface substitution —
// "**T is an interface** that has I in its extension chain — either I is named
// directly in T's inheritance clause, or T extends an interface that itself
// conforms to I" — and the §14.2 worked example states outright that "`where T:
// IComparable<int>` therefore admits a mix of primitive, class, record, and
// **interface** substitutions".  So `Array<IFoo>.Sort()` for `interface IFoo:
// IComparable<IFoo>` is a document-admissible shape whose order must be the
// user's `CompareTo`.
//
// THE TWO DEFECTS.  They sat in different layers:
//   * Sort()   — codegen (LlvmBackend_Calls.cpp).  An interface element is not
//     a Class, so the #1195 `classCompareMs` probe missed it; it lowers to
//     ptrTy(), so `isStringElem` claimed it and `bz$strcmp` strcmp'd each
//     element's VTABLE POINTER as a char*.  All instances of one class share
//     one vtable, so every pair compared equal, the array came back in
//     insertion order and CompareTo was never called: a SILENT WRONG ANSWER.
//   * Sorted() — sema (ExprTypecheck_Calls.cpp).  Its comparability gate
//     accepted only primitives and *classes* naming IComparable, so the same
//     document-admissible shape was FALSELY REJECTED and never reached codegen
//     at all.  (The issue reported both as the same silent wrong answer; that
//     is corrected here — measured on 0e554e3.)
//
// The controls below are the load-bearing half: the fix must not pull any other
// element category into the new interface arm.  In particular a `string`
// element must keep its §9.6 ordinal comparison — routing one into itable
// dispatch would read `char* data` as a vtable, which is exactly #2752's crash.
//
// ASAN ALLOCATION.  The whole hazard of this fix is reading word 0 of an
// element as a vtable, so generated-code ASan (runAotChecked) is spent first on
// the four cases that actually REACH the new `bz$ifacecmp$` thunk, and then on
// the controls that could only fail by a wrong routing.  Two of the eleven
// cases are deliberately plain: the invariance control is a compile-reject
// case, so it never runs and ASan is meaningless for it; and
// control_string_element_still_ordinal is plain for a documented, independently
// proven pre-existing leak — its reason is stated at the case.
//
// ⚠ MEASURED LIMIT: every case here is SAME-MODULE.  With the interface and its
// implementor in an imported library the defect persists (the repro returns the
// pre-fix 32, not 18), because the imported interface's baseTypeRefs does not
// carry `IComparable<IFoo>`.  Filed separately; not covered by these tests.

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
// Generated-code ASan/LSan: a wrongly routed element (a vtable read as a
// bz_string, or a string read as a vtable) shows up as a memory error here
// rather than as a value coincidence.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// #2790: asserting on diagnostic TEXT, not the coarse "did not run" — runAot
// synthesizes an Error for a runtime crash too, and these shapes previously
// COMPILED, so the distinction matters.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
const char* kSortNeedle = "requires T to implement";

// `interface IFoo: IComparable<IFoo>` — the §14.2 rule-3 shape, with the
// self-recurrence stdlib §4.4's `where T: IComparable<T>` requires.
const char* IFOO =
    "module M; "
    "public interface IFoo: IComparable<IFoo> "
    "  method Rank(): int; "
    "end IFoo; ";

// One class implementing it, ascending by Rank.
const char* NODE =
    "class Node: IFoo "
    "  private _r: int; "
    "  public constructor(r: int) _r := r; end constructor; "
    "  public method Rank(): int => _r; "
    "  public method CompareTo(other: IFoo): int => _r - other.Rank(); "
    "end Node; ";
}  // namespace

// #2768 / stdlib §4.4 + §2.8 / spec §14.2 rule 3: the reported repro.  Three
// elements inserted 3,1,2; ascending CompareTo must yield 1,2,3.
//
// The result is packed in base 3 (arr[0]*9 + arr[1]*3 + arr[2]) so that BOTH
// the expected and the defective value stay under 256 and are not confused by
// runAot's 8-bit WEXITSTATUS masking: sorted 1,2,3 -> 18, the defect's
// untouched insertion order 3,1,2 -> 32.  (The issue quoted 123 vs 312, which
// masks to 123 vs 56 — the source of the "56" note there.)
BEZEL_TEST(array_sort_iface_elem_2768, sort_iface_element_orders_by_compareto) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    using var c := new Node(2); "
        "    var arr: Array<IFoo> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].Rank() * 9 + arr[1].Rank() * 3 + arr[2].Rank(); "
        "  end Main; "
        "end C; "), 18);
}

// #2768: the strong form.  TWO distinct classes implement IFoo, so the array is
// genuinely polymorphic and the elements do NOT share one vtable — which kills
// the "every pair compared equal" accident that made the defect look like a
// no-op.  CompareTo orders DESCENDING and increments a counter, so:
//   * a descending result can only come from the user's CompareTo (no
//     pointer/vtable/insertion order produces it), and
//   * the counter proves CompareTo actually RAN, rather than the order having
//     come out right by luck.
// Input 1,3,2 -> descending 3,2,1 packed base 4 = 3*16+2*4+1 = 57, +64 for the
// counter = 121.  Pre-fix the array is untouched: 1,3,2 = 30, counter 0 -> 30.
//
// Under ASan: this is the case where two DIFFERENT vtables flow through the
// thunk, so it is the strongest place to catch a bad word-0 read.
BEZEL_TEST(array_sort_iface_elem_2768,
           sort_iface_polymorphic_descending_and_compareto_ran) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) +
        "class Counter public static _n: int := 0; end Counter; "
        "class Lo: IFoo "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IFoo): int "
        "    Counter._n := Counter._n + 1; return other.Rank() - _r; "
        "  end CompareTo; "
        "end Lo; "
        "class Hi: IFoo "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IFoo): int "
        "    Counter._n := Counter._n + 1; return other.Rank() - _r; "
        "  end CompareTo; "
        "end Hi; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Lo(1); "
        "    using var b := new Hi(3); "
        "    using var c := new Lo(2); "
        "    var arr: Array<IFoo> := [a, b, c]; "
        "    arr.Sort(); "
        "    var p: int := arr[0].Rank()*16 + arr[1].Rank()*4 + arr[2].Rank(); "
        "    if Counter._n > 0 then p := p + 64; end; "
        "    return p; "
        "  end Main; "
        "end C; "), 121);
}

// #2768 / stdlib §4.4: the comparerless `Sorted()` on the same shape.  This was
// a SEMA false reject ("Array<T>.Sorted() requires T to implement
// IComparable<T>") — the shape never reached codegen.  Asserts both halves of
// §4.4's past-participle contract: the returned copy is sorted (1,2,3 packed
// base 4 = 27) AND the receiver is left unchanged (still 3,1,2 -> +64) = 91.
BEZEL_TEST(array_sort_iface_elem_2768,
           sorted_iface_element_sorts_copy_and_leaves_receiver) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    using var c := new Node(2); "
        "    var arr: Array<IFoo> := [a, b, c]; "
        "    using var s := arr.Sorted(); "
        "    var p: int := s[0].Rank()*16 + s[1].Rank()*4 + s[2].Rank(); "
        "    if arr[0].Rank() = 3 and arr[1].Rank() = 1 and arr[2].Rank() = 2 "
        "      then p := p + 64; end; "
        "    return p; "
        "  end Main; "
        "end C; "), 91);
}

// #2768 / spec §12: the implementing class INHERITS CompareTo from a base
// class rather than declaring it.  The itable is built per (class, interface)
// with findInterfaceImpl walking bases, so the derived class's slot must hold
// the base's method.  3,1,2 -> 18.
BEZEL_TEST(array_sort_iface_elem_2768, sort_iface_element_inherited_compareto) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) +
        "class Base: IFoo "
        "  protected _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IFoo): int => _r - other.Rank(); "
        "end Base; "
        "class Derived: Base "
        "  public constructor(r: int) base(r); end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Derived(3); "
        "    using var b := new Derived(1); "
        "    using var c := new Derived(2); "
        "    var arr: Array<IFoo> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].Rank() * 9 + arr[1].Rank() * 3 + arr[2].Rank(); "
        "  end Main; "
        "end C; "), 18);
}

// CONTROL (stdlib §9.6 / spec §4.6) — a `string` element keeps its ordinal
// comparison.  A string is a runtime-managed reference whose word 0 is
// `char* data`, NOT a vtable, so routing it into itable dispatch would read
// data[1] as an itmap: #2752's crash.
//
// Plain runAot, not runAotChecked, for an INDEPENDENT pre-existing leak proven
// by differential probe: an `Array<string>` collection-literal local leaks 176
// bytes in 2 allocations (bz_array_alloc) — byte-for-byte the SAME leak with
// the `arr.Sort()` line removed entirely.  LeakSanitizer overrides the process
// exit code to 1, which would mask the value this test is about.  Filed
// separately.  The memory-error half of the oracle is not lost: ASan aborts at
// a bad READ, so the run reaching LSan's end-of-process leak check is itself
// evidence that no out-of-bounds access occurred on this path.
BEZEL_TEST(array_sort_iface_elem_2768, control_string_element_still_ordinal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var arr: Array<string> := [\"c\", \"a\", \"b\"]; "
        "    arr.Sort(); "
        "    var r: int := 0; "
        "    if arr[0] = \"a\" then r := r + 1; end; "
        "    if arr[1] = \"b\" then r := r + 2; end; "
        "    if arr[2] = \"c\" then r := r + 4; end; "
        "    return r; "
        "  end Main; "
        "end C; "), 7);
}

// CONTROL (#1195 / spec §14.2) — a CLASS element naming IComparable<Self>
// keeps the bz$classcmp$ vtable thunk; the interface arm must not steal it.
BEZEL_TEST(array_sort_iface_elem_2768, control_class_element_still_compareto) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Node: IComparable<Node> "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: Node): int => _r - other.Rank(); "
        "end Node; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    using var c := new Node(2); "
        "    var arr: Array<Node> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].Rank() * 9 + arr[1].Rank() * 3 + arr[2].Rank(); "
        "  end Main; "
        "end C; "), 18);
}

// CONTROL — an `int` element keeps bz$intcmp.  3,1,2 -> 18.
BEZEL_TEST(array_sort_iface_elem_2768, control_int_element_still_ascending) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var arr: Array<int> := [3, 1, 2]; "
        "    arr.Sort(); "
        "    return arr[0] * 9 + arr[1] * 3 + arr[2]; "
        "  end Main; "
        "end C; "), 18);
}

// CONTROL — THE ANTI-MIS-ROUTE GUARD.  `Array<IComparable<string>>` holds
// `string`s: stdlib §9.6 declares `public class String: IEnumerable<char>,
// IComparable<string>, IEquatable<string>`, so a string DOES inhabit this
// interface-typed slot.  The fix's predicate is the §4.4 constraint itself —
// the element interface must name `IComparable<Self>` — and `IComparable<string>`
// would have to name `IComparable<IComparable<string>>` to qualify, so it does
// not, and no string is ever handed to the itable thunk.  Run under ASan: had
// the predicate been the looser "any IComparable in the chain", this test would
// fail on a heap-buffer-overflow rather than on a value.
BEZEL_TEST(array_sort_iface_elem_2768,
           control_icomparable_string_element_rejected_2790) {
    // ⚠ UPDATED BY #2790.  This asserted that the program COMPILES and returns
    // 7.  stdlib §4.4 constrains `where T: IComparable<T>`, and here T is
    // `IComparable<string>`, which does not implement
    // `IComparable<IComparable<string>>` — so the call must be REJECTED.  It was
    // accepted only because the parameterless `Sort()` had NO gate at all while
    // `Sorted()` did; `Array<IComparable<string>>.Min()` already rejected this
    // exact element (measured during #2770).  The case still guards what it was
    // written to guard — that a string element is never itable-routed — because
    // a routing change would COMPILE and fail this assertion.
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; class C "
        "  public static method Main(): int "
        "    var arr: Array<IComparable<string>> := [\"c\", \"a\", \"b\"]; "
        "    arr.Sort(); "
        "    return 7; "
        "  end Main; "
        "end C; ", kSortNeedle));
}

// CONTROL — an interface element with NO IComparable in its chain
// (`Array<IDisposable>`, spec §28.2's implicit conformance).  stdlib §4.4 does
// not admit it and the fix does not claim it: behaviour is left exactly as it
// was, memory-safe and without a new rejection.  Pinned so a later widening of
// the predicate cannot silently start dispatching here.
BEZEL_TEST(array_sort_iface_elem_2768,
           control_non_comparable_interface_element_rejected_2790) {
    // ⚠ UPDATED BY #2790, for the same reason its Min() twin was updated by
    // #2784: `IDisposable` names no `IComparable`, so stdlib §4.4's constraint
    // is unsatisfied and the call must be REJECTED.  It compiled only because
    // the parameterless `Sort()` had no gate.
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class Node public constructor() end constructor; end Node; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(); "
        "    using var b := new Node(); "
        "    var arr: Array<IDisposable> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "
        "  end Main; "
        "end C; ", kSortNeedle));
}

// CONTROL — INVARIANCE.  `interface I<T>: IComparable<I<int>>` names
// `IComparable<I<int>>`, NOT `IComparable<I<string>>`, and spec §12.9 makes
// interface type arguments INVARIANT — so `I<string>` does not conform to
// `IComparable<I<string>>` and stdlib §4.4's extension does not apply to
// `Array<I<string>>`.  It must stay rejected.
//
// This pins the #1867 convention the predicate follows: the element type is
// compared to IComparable's argument with §14.2's exact, component-wise
// identity (sema::typeRefEqual), so the argument's OWN generic arguments are
// compared too.  A `declaration ==` test would compare only the head `I` and
// wrongly admit this — the same laxity recorded against the class arm as #2772
// item 2.  Asserted on diagnostic TEXT rather than the coarse reject, because
// runAot synthesizes an Error for a runtime SIGSEGV too and so cannot by itself
// tell a genuine rejection from a crash.
BEZEL_TEST(array_sort_iface_elem_2768,
           control_invariant_generic_argument_stays_rejected) {
    auto r = runAot(
        "module M; "
        "public interface I<T>: IComparable<I<int>> "
        "  method Rank(): int; "
        "end I; "
        "class Node: I<string> "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: I<int>): int => _r - other.Rank(); "
        "end Node; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    var arr: Array<I<string>> := [a, b]; "
        "    using var s := arr.Sorted(); "
        "    return 7; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool sawSortedConstraint = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("Sorted() requires T to implement IComparable<T>") !=
            std::string::npos)
            sawSortedConstraint = true;
    BEZEL_EXPECT_TRUE(sawSortedConstraint);
}

// CONTROL — a NULLABLE interface element.  spec §14.2: "A type-bound
// constraint — naming a class or interface — implies non-null T", so `IFoo?`
// does not satisfy `where T: IComparable<T>` and the interface arm excludes it.
// That exclusion is deliberate: itable-dispatching a slot that may hold `null`
// would turn today's memory-safe no-op into a null deref.  (That sema admits
// this call at all is a separate gap, filed — this test only pins that the fix
// does not make it worse.)
BEZEL_TEST(array_sort_iface_elem_2768,
           control_nullable_interface_element_rejected_2790) {
    // ⚠ UPDATED BY #2790.  spec §14.2: "A type-bound constraint — naming a
    // class or interface — implies non-null T", so `IFoo?` does not satisfy
    // §4.4's `where T: IComparable<T>` and must be REJECTED — as the identical
    // `Array<IFoo?>.Min()` shape already was (#2770).  It compiled only because
    // the parameterless `Sort()` had no gate.  The exclusion is deliberate:
    // itable-dispatching a slot that may hold `null` would be a null deref.
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    var arr: Array<IFoo?> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "
        "  end Main; "
        "end C; ", kSortNeedle));
}
