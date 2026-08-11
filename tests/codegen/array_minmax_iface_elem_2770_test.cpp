// Spec-derived regression tests for #2770 (v0.240): `Array<T>.Min()` /
// `.Max()` when T is an INTERFACE type.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §5.1 ("No-selector aggregation
// overloads") declares the comparerless forms only inside
//     public extension IEnumerable<T> where T: IComparable<T>
//       public method Min(): T;              // throws if empty
//       public method Max(): T;              // throws if empty
//     end;
// and stdlib §2.8 fixes the ordering meaning: "`a.CompareTo(b)` returns a
// negative number if `a < b`, zero if equal, a positive number if `a > b`."
// The selector forms are declared in stdlib §5 as
//     public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//
// spec §14.2's interface-conformance rule 3 admits an interface substitution —
// "**T is an interface** that has I in its extension chain — either I is named
// directly in T's inheritance clause, or T extends an interface that itself
// conforms to I" — and the §14.2 worked example states outright that "`where T:
// IComparable<int>` therefore admits a mix of primitive, class, record, and
// **interface** substitutions".  So `Array<IFoo>.Min()` for `interface IFoo:
// IComparable<IFoo>` is a document-admissible shape that must COMPILE and must
// return the element least under the user's `CompareTo`.
//
// THE DEFECT — a compiler ICE, in CODEGEN only.  Unlike #2768's Sort()/Sorted()
// pair, both layers were NOT split here; measured on 186d656:
//   * sema (ExprTypecheck_Calls.cpp ~L4525) deliberately ADMITS an interface
//     element — `ptrComparableElem` is `Class || Interface || non-null string`
//     and the comment says "Codegen dispatches the comparison via
//     CompareTo/strcmp".  So sema is NOT a false reject here.
//   * codegen (LlvmBackend_Calls.cpp) probed for the element's `CompareTo` with
//     `findMethodByName`, which walks CLASS bases only (`baseClassOf`) and
//     additionally demands `methodFns_.count(ms)` — a COMPILED BODY.  An
//     interface element's `CompareTo` is declared on `IComparable` and has no
//     body, so the probe missed, the pointer-comparable arm was skipped, and
//     the numeric path below it folded the comparison with `icmp` between a
//     pointer and an i32.  LLVM's `ICmpInst::AssertOK` fired and `bezelc`
//     ABORTED:
//       Assertion `getOperand(0)->getType() == getOperand(1)->getType() &&
//       "Both operands to ICmp instruction are not of the same type!"' failed.
//
// This refutes the note carried from #2752's review into #2768, that Min/Max
// "route through emitMethodCall and are therefore already correct" for an
// interface element.  The predicate there does name `Interface`, but the shape
// did not compile at all.
//
// THE FIX reuses #2768's `bz$ifacecmp$<Iface>` thunk UNCHANGED — the Min/Max
// loop already holds slot ADDRESSES (`bz_array_ptr_at`'s result and the
// accumulator alloca), which is exactly that thunk's `bz_array_sort` ABI.  The
// arm is selected from the STATIC element type by `selfComparableIfaceCompareTo`
// — the exact §5.1 constraint, matching `IComparable` by IDENTITY (#1867) and
// its argument with `sema::typeRefEqual` — so, as in #2768, no other element
// category can be mis-routed into itable dispatch.  In particular no `string`
// can: stdlib §9.6 declares `public class String: IEnumerable<char>,
// IComparable<string>, IEquatable<string>`, none of which names
// `IComparable<Self>`.
//
// The fix also stops the ICE for the element types the predicate correctly
// DECLINES.  Sema's §5.1 gate scans the element's DIRECT AST base list for the
// NAME `IComparable` without checking its type argument or nullability, so it
// admits shapes the interface arm declines — and each of those landed on the
// same assertion.  Codegen now reports a spec-cited Error instead of aborting.
// Three such shapes are pinned below as reject controls.  Every one of them
// ABORTED the compiler before this fix, so none is a new rejection of working
// code.
//
// ASAN ALLOCATION.  The hazard of this fix is reading word 0 of an element as a
// vtable, so generated-code ASan (runAotChecked) is spent on the four cases
// that actually REACH the new dispatch, including the two-distinct-vtables case
// and the selector projection.  The reject controls never run, so ASan is
// meaningless for them; the string control is plain runAot for the independent
// pre-existing `Array<string>` literal leak recorded on #2774, which would let
// LeakSanitizer override the exit code this test asserts on.
//
// ⚠ MEASURED LIMIT: every case here is SAME-MODULE.  #2776 records that the
// shared predicate does not reach a CROSS-MODULE interface, because the
// imported interface's `baseTypeRefs` does not carry its `IComparable` base.

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
// Generated-code ASan/LSan: a wrongly routed element (a string's `char* data`
// read as a vtable, or a vtable read as a bz_string) surfaces as a memory error
// here rather than as a value coincidence.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// True when some reported diagnostic contains `needle`.  Asserting on
// diagnostic TEXT is required for a reject case: runAot synthesizes an Error
// for a runtime SIGSEGV too, so the coarse "did not run" cannot by itself tell
// a genuine rejection from a crash — and the defect under test here was an
// abort, exactly the confusion that would hide.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `interface IFoo: IComparable<IFoo>` — the §14.2 rule-3 shape, carrying the
// self-recurrence stdlib §5.1's `where T: IComparable<T>` requires.
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

// #2770 / stdlib §5.1 + §2.8 / spec §14.2 rule 3: the reported repro, which
// ABORTED the compiler on LLVM's ICmp assertion before this fix.
//
// Elements 3,1,2 with an ascending CompareTo: Min is 1 and Max is 3.  Packed as
// Min*10 + Max = 13, well under runAot's 8-bit WEXITSTATUS mask, so no
// truncation is involved and no decomposition is needed.
BEZEL_TEST(array_minmax_iface_elem_2770, min_max_iface_element_dispatch_compareto) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    using var c := new Node(2); "
        "    var arr: Array<IFoo> := [a, b, c]; "
        "    return arr.Min().Rank() * 10 + arr.Max().Rank(); "
        "  end Main; "
        "end C; "), 13);
}

// #2770: the strong form.  TWO distinct classes implement IFoo, so the array is
// genuinely polymorphic and the elements do NOT share a single vtable; the
// CompareTo orders DESCENDING and increments a counter.  Therefore:
//   * a descending selection can only come from the user's CompareTo — no
//     pointer, vtable, or positional accident produces it, and
//   * the counter proves CompareTo actually RAN rather than the answer having
//     come out right by luck (the review point that a value alone is too weak).
// Ranks 1,3,2 under `other.Rank() - _r`: the least element is rank 3 and the
// greatest is rank 1, so Min*10 + Max = 31, +64 for the counter = 95.
//
// Under ASan this is the strongest case: two DIFFERENT vtables flow through the
// new thunk, so a bad word-0 read shows up as a memory error.
BEZEL_TEST(array_minmax_iface_elem_2770,
           minmax_iface_polymorphic_descending_and_compareto_ran) {
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
        "    var p: int := arr.Min().Rank() * 10 + arr.Max().Rank(); "
        "    if Counter._n > 0 then p := p + 64; end; "
        "    return p; "
        "  end Main; "
        "end C; "), 95);
}

// #2770 / spec §12: the implementing class INHERITS CompareTo from a base class
// rather than declaring it.  The itable is built per (class, interface) with
// findInterfaceImpl walking bases, so the derived class's slot must hold the
// base's method.  3,1,2 -> Min 1, Max 3 -> 13.
BEZEL_TEST(array_minmax_iface_elem_2770, min_max_iface_element_inherited_compareto) {
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
        "    return arr.Min().Rank() * 10 + arr.Max().Rank(); "
        "  end Main; "
        "end C; "), 13);
}

// #2770 / stdlib §5: the SELECTOR overload `Min<U>(selector: (T) -> U): U
// where U: IComparable<U>` at an interface U.  The same `findMethodByName`
// probe sits at that sibling site, so an interface projection was equally
// invisible — MEASURED to fail differently there (the #2491 "return-value
// expression failed to lower" internal diagnostic, not the ICmp abort), while a
// CLASS projection at the same site compiled and ran correctly.  Same root
// cause, same commit.  3,1,2 -> Min 1, Max 3 -> 13.
BEZEL_TEST(array_minmax_iface_elem_2770, min_max_iface_selector_projection) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    using var c := new Node(2); "
        "    var arr: Array<IFoo> := [a, b, c]; "
        "    return arr.Min(lambda(x: IFoo) => x).Rank() * 10 "
        "         + arr.Max(lambda(x: IFoo) => x).Rank(); "
        "  end Main; "
        "end C; "), 13);
}

// CONTROL (#1195 / spec §14.2) — a CLASS element naming IComparable<Self> keeps
// the existing emitMethodCall dispatch; the interface arm is tried only when
// that probe returns nothing, so this path is bit-identical.  Measured 13 both
// before and after the fix.
BEZEL_TEST(array_minmax_iface_elem_2770, control_class_element_still_compareto) {
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
        "    return arr.Min().Rank() * 10 + arr.Max().Rank(); "
        "  end Main; "
        "end C; "), 13);
}

// CONTROL — an `int` element keeps the numeric path this fix must not disturb.
// 3,1,2 -> Min 1, Max 3 -> 13.
BEZEL_TEST(array_minmax_iface_elem_2770, control_int_element_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var arr: Array<int> := [3, 1, 2]; "
        "    return arr.Min() * 10 + arr.Max(); "
        "  end Main; "
        "end C; "), 13);
}

// CONTROL (stdlib §9.6 / spec §4.6) — a `string` element keeps its ordinal
// comparison.  A string is a runtime-managed reference whose word 0 is
// `char* data`, NOT a vtable, so routing one into itable dispatch would read
// data[1] as an itmap: #2752's crash.  The predicate structurally excludes it
// (see the header note), and this pins that.
//
// Plain runAot, not runAotChecked, for the INDEPENDENT pre-existing
// `Array<string>` collection-literal leak recorded on #2774: LeakSanitizer
// overrides the process exit code, which would mask the value under test.  The
// memory-error half of the oracle is not lost — ASan aborts at a bad READ, so a
// mis-route would fault before any leak check.
BEZEL_TEST(array_minmax_iface_elem_2770, control_string_element_still_ordinal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var arr: Array<string> := [\"c\", \"a\", \"b\"]; "
        "    var r: int := 0; "
        "    if arr.Min() = \"a\" then r := r + 1; end; "
        "    if arr.Max() = \"c\" then r := r + 2; end; "
        "    return r; "
        "  end Main; "
        "end C; "), 3);
}

// CONTROL — THE ANTI-MIS-ROUTE GUARD.  `Array<IComparable<string>>` holds
// `string`s: stdlib §9.6 declares `public class String: IEnumerable<char>,
// IComparable<string>, IEquatable<string>`, so a string DOES inhabit this
// interface-typed slot.  Had the interface predicate been the looser "any
// IComparable in the chain", a `char* data` word would have been dispatched as
// a vtable — #2752's crash.
//
// MEASURED: this shape never reaches codegen at all.  Sema's own §5.1 gate
// rejects it first with the #1198 diagnostic, because `IComparable`'s AST base
// list does not itself name `IComparable`.  This test therefore pins the
// EXISTING sema rejection, unchanged by this commit (which touches no sema
// code), and asserts on the #1198 text specifically so that a future widening
// which let the shape through to codegen would fail here rather than silently
// start dispatching a string's data pointer as a vtable.
BEZEL_TEST(array_minmax_iface_elem_2770,
           control_icomparable_string_element_not_itable_routed) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; class C "
        "  public static method Main(): int "
        "    var arr: Array<IComparable<string>> := [\"c\", \"a\", \"b\"]; "
        "    var m := arr.Min(); "
        "    return 7; "
        "  end Main; "
        "end C; ",
        "#1198"));
}

// An interface element with NO IComparable in its chain (`Array<IDisposable>`).
// stdlib §5.1 does not admit it, so it must be REJECTED.
//
// ⚠ UPDATED BY #2784.  As originally written this case asserted that the
// program COMPILES and returns 7, described as "MEASURED: … it reaches neither
// the new interface arm … nor the new reject guard, so this commit cannot have
// changed it."  That measurement was accurate, but it pinned a DEFECT: the call
// violates §5.1's `where T: IComparable<T>` and was being accepted silently,
// returning a non-null element chosen under no defined ordering, while the
// user-defined analogue was correctly rejected.  #2784 established that; the
// reject guard is now category-blind (it no longer requires the element to
// carry a `declaration`, which a BUILTIN interface never has — root cause
// #2789), so this shape now reports the §5.1 diagnostic.
//
// The case still serves its original purpose.  It was written so "a later
// widening of the predicate cannot silently start itable-dispatching here", and
// it still catches exactly that: a widening that dispatched `IDisposable`
// through the itable would COMPILE, and this assertion — which demands the
// diagnostic — would fail.  Only the pinned outcome changed, from "silently
// accepted" to "rejected"; what is being guarded against did not.
BEZEL_TEST(array_minmax_iface_elem_2770, control_non_comparable_interface_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class Node public constructor() end constructor; end Node; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(); "
        "    using var b := new Node(); "
        "    var arr: Array<IDisposable> := [a, b]; "
        "    var m := arr.Min(); "
        "    return 7; "
        "  end Main; "
        "end C; ",
        "requires the element type to satisfy"));
}

// CONTROL — a NULLABLE interface element.  spec §14.2: "A type-bound constraint
// — naming a class or interface — implies non-null T", so `IFoo?` does not
// satisfy §5.1's `where T: IComparable<T>` and the interface arm excludes it.
// The exclusion is deliberate: itable-dispatching a slot that may hold `null`
// would be a null deref.  MEASURED to abort the compiler before this fix; it is
// now the spec-cited diagnostic, so this is not a new rejection of working
// code.
BEZEL_TEST(array_minmax_iface_elem_2770, control_nullable_interface_element_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    var arr: Array<IFoo?> := [a, b]; "
        "    var m := arr.Min(); "
        "    return 7; "
        "  end Main; "
        "end C; ",
        "#2770"));
}

// CONTROL — INVARIANCE.  `interface IG<T>: IComparable<IG<int>>` names
// `IComparable<IG<int>>`, NOT `IComparable<IG<string>>`, and spec §12.9 makes
// interface type arguments INVARIANT — so `IG<string>` does not satisfy §5.1's
// self-recurrence.  This pins the #1867 convention the shared predicate
// follows: the element is compared to IComparable's argument with §14.2's
// exact, component-wise identity (`sema::typeRefEqual`), which compares the
// argument's OWN generic arguments too.  A bare `declaration ==` test would
// compare only the head `IG` and wrongly admit this — and would then hand the
// itable thunk an interface whose CompareTo takes a DIFFERENT type.
// MEASURED to abort the compiler before this fix.
BEZEL_TEST(array_minmax_iface_elem_2770,
           control_invariant_generic_argument_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "public interface IG<T>: IComparable<IG<int>> "
        "  method Rank(): int; "
        "end IG; "
        "class Node: IG<string> "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IG<int>): int => _r - other.Rank(); "
        "end Node; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    var arr: Array<IG<string>> := [a, b]; "
        "    var m := arr.Min(); "
        "    return 7; "
        "  end Main; "
        "end C; ",
        "#2770"));
}
