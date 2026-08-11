// Regression tests for #2782 — `Math.Min` / `Math.Max` / `Math.Clamp` with an
// INTERFACE argument.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §2.2 ("`Math`", v0.240) declares the
// comparing forms only as
//     public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
//     public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
// and spec §14.2 conformance rule 3 admits an interface substitution — "**T is
// an interface** that has I in its extension chain — either I is named directly
// in T's inheritance clause, or T extends an interface that itself conforms to
// I" — with the §14.2 worked example stating outright that
// `where T: IComparable<int>` "admits a mix of primitive, class, record, and
// **interface** substitutions".  stdlib §2.8 fixes the ordering meaning:
// "`a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a
// positive number if `a > b`."
//
// THE DEFECT WAS A SILENT WRONG VALUE, not a stopped build — strictly worse
// than the ICE on the sibling `Array<T>.Min()` shape (#2770).  The program
// COMPILED (rc=0, only the #2491 "internal:" Warning) and returned 0 where 37
// was required.
//
// TWO LAYERS, and the second is why the first alone was not enough:
//   * sema (ExprTypecheck_Calls.cpp ~L8232) listed Record|Enum|Class and omitted
//     Interface, so the call was left mistyped (defaulting to int) and member
//     access on the result could not resolve.
//   * codegen (LlvmBackend_Calls.cpp ~L8039) omitted Interface from the same
//     kind test, so `comparableCompareTo` returned null and the Min/Max/Clamp
//     arms fell through to the FLOAT path.
// An ANNOTATED `var lo: IFoo := Math.Min(x, y);` supplies the type and so
// MASKED the sema half — which is exactly why min_max_iface_unannotated_local
// and min_max_iface_direct_receiver below are separate cases from it.
//
// MEASURED PRE-FIX (`7ca80c5`, by reverting the fix and re-running):
//   Math.Min(x,y).Rank()*10 + Math.Max(x,y).Rank()   0  (required 37)
//   var t := Math.Min(x, y); t.Rank()                0  (required 3)
//   invariance-violating interface argument          0  + internal Warning
// and post-fix 37, 3, and a clean §12.9 rejection respectively.
//
// Not covered here, filed instead — both measured to be PRE-EXISTING and
// bit-identical across this fix (verified by reverting it and re-measuring):
//   #2785  Math.Min("b", "a") — a STRING argument aborts the compiler
//          (`SIToFP source must be integer`), though stdlib §9.6 makes string
//          IComparable<string>.
//   #2786  Math.Min over IDisposable — a BUILTIN interface argument aborts the
//          same way instead of reporting the §2.2 constraint violation.

#include <string>

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
// Generated-code ASan/LSan.  The hazard in the new arm is reading word 0 of an
// operand as a vtable, so a mis-route surfaces as a memory error here rather
// than as a value coincidence.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Asserting on diagnostic TEXT is required for a reject case: runAot
// synthesizes an Error for a runtime SIGSEGV too, so the coarse "did not run"
// cannot by itself distinguish a genuine rejection from a crash.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `interface IFoo: IComparable<IFoo>` — the §14.2 rule-3 shape carrying the
// self-recurrence stdlib §2.2's `where T: IComparable<T>` requires.
const char* IFOO =
    "module M; "
    "public interface IFoo: IComparable<IFoo> "
    "  method Rank(): int; "
    "end IFoo; ";

const char* NODE =
    "class Node: IFoo "
    "  private _r: int; "
    "  public constructor(r: int) _r := r; end constructor; "
    "  public method Rank(): int => _r; "
    "  public method CompareTo(other: IFoo): int => _r - other.Rank(); "
    "end Node; ";

// A SECOND, unrelated implementor — a distinct vtable, so a result that tracks
// Rank can only have come from real itable dispatch and not from selecting a
// fixed operand.
const char* LEAF =
    "class Leaf: IFoo "
    "  private _r: int; "
    "  public constructor(r: int) _r := r; end constructor; "
    "  public method Rank(): int => _r; "
    "  public method CompareTo(other: IFoo): int => _r - other.Rank(); "
    "end Leaf; ";
}  // namespace

// #2782 / stdlib §2.2 + §2.8 / spec §14.2 rule 3 — the reported repro, in the
// spelling that has no type annotation to mask the sema half.  Ranks 7 and 3
// with an ascending CompareTo: Min is 3 and Max is 7, packed as Min*10 + Max =
// 37.  Both that and every wrong ordering of one-digit ranks stay under 256, so
// runAot's 8-bit WEXITSTATUS mask is not involved at all.
BEZEL_TEST(math_minmax_iface_arg_2782, min_max_iface_direct_receiver) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(7); "
        "    using var b := new Node(3); "
        "    var x: IFoo := a; "
        "    var y: IFoo := b; "
        "    return Math.Min(x, y).Rank() * 10 + Math.Max(x, y).Rank(); "
        "  end Main; "
        "end C; "), 37);
}

// The SEMA half in isolation: an UNANNOTATED local.  With the call mistyped as
// int, `t.Rank()` could not resolve and the whole expression degraded to zero.
// An annotated `var t: IFoo := …` passes even with only the codegen half
// applied, so this case is what pins the sema fix.
BEZEL_TEST(math_minmax_iface_arg_2782, min_max_iface_unannotated_local) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(7); "
        "    using var b := new Node(3); "
        "    var x: IFoo := a; "
        "    var y: IFoo := b; "
        "    var t := Math.Min(x, y); "
        "    return t.Rank(); "
        "  end Main; "
        "end C; "), 3);
}

// The ANNOTATED spelling — the one that masked the sema half.  Kept so that a
// future regression of either layer alone is still caught by at least one case.
BEZEL_TEST(math_minmax_iface_arg_2782, min_max_iface_annotated_local) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(7); "
        "    using var b := new Node(3); "
        "    var x: IFoo := a; "
        "    var y: IFoo := b; "
        "    var lo: IFoo := Math.Min(x, y); "
        "    var hi: IFoo := Math.Max(x, y); "
        "    return lo.Rank() * 10 + hi.Rank(); "
        "  end Main; "
        "end C; "), 37);
}

// TWO DISTINCT IMPLEMENTORS — two distinct vtables.  A result that tracks Rank
// across different concrete types can only come from dispatching each operand's
// own `CompareTo` through the itable.
BEZEL_TEST(math_minmax_iface_arg_2782, min_max_iface_two_distinct_vtables) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE + LEAF +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(7); "
        "    using var b := new Leaf(3); "
        "    var x: IFoo := a; "
        "    var y: IFoo := b; "
        "    return Math.Min(x, y).Rank() * 10 + Math.Max(x, y).Rank(); "
        "  end Main; "
        "end C; "), 37);
}

// stdlib §2.2 also declares `Clamp<T>`, which reaches the SAME comparator pair
// from two further call sites.  Fixing Min/Max and leaving Clamp broken would
// have left the defect half-closed, so both boundaries are pinned: a value
// inside the range is returned unchanged...
BEZEL_TEST(math_minmax_iface_arg_2782, clamp_iface_within_range) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(1); "
        "    using var b := new Node(5); "
        "    using var c := new Node(9); "
        "    var lo: IFoo := a; "
        "    var v: IFoo := b; "
        "    var hi: IFoo := c; "
        "    return Math.Clamp(v, lo, hi).Rank(); "
        "  end Main; "
        "end C; "), 5);
}

// ...and a value below the low bound comes back as the low bound.
BEZEL_TEST(math_minmax_iface_arg_2782, clamp_iface_below_low_bound) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(4); "
        "    using var b := new Node(1); "
        "    using var c := new Node(9); "
        "    var lo: IFoo := a; "
        "    var v: IFoo := b; "
        "    var hi: IFoo := c; "
        "    return Math.Clamp(v, lo, hi).Rank(); "
        "  end Main; "
        "end C; "), 4);
}

// REJECT CASE — an interface whose `IComparable` argument is NOT itself.
// §12.9: `IComparable` is invariant, so `interface IG<T>: IComparable<IG<int>>`
// does not satisfy `where T: IComparable<T>` for an `IG<string>` argument.
//
// This case is why sema was given the SAME exact test as codegen rather than the
// looser `isSubtype(decl, IComparable)`: codegen's arm declines this shape, and
// an interface it declines would fall through to the float path's
// `sitofp ptr … to double`.  MEASURED to return a silent 0 before the fix.
BEZEL_TEST(math_minmax_iface_arg_2782,
           control_invariance_violating_interface_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "public interface IG<T>: IComparable<IG<int>> "
        "  method Rank(): int; "
        "end IG; "
        "class G<T>: IG<T> "
        "  public constructor() end constructor; "
        "  public method Rank(): int => 1; "
        "  public method CompareTo(other: IG<int>): int => 0; "
        "end G; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new G<string>(); "
        "    using var b := new G<string>(); "
        "    var x: IG<string> := a; "
        "    var y: IG<string> := b; "
        "    return Math.Min(x, y).Rank(); "
        "  end Main; "
        "end C; ",
        "does not satisfy the constraint"));
}

// CONTROL — a CLASS argument, the #1148 path.  Unchanged by this fix: the
// interface arm is tried only after the existing kind test returns nothing, so
// every shape that worked before takes a bit-identical path.
BEZEL_TEST(math_minmax_iface_arg_2782, control_class_argument_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string(IFOO) + NODE +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(7); "
        "    using var b := new Node(3); "
        "    return Math.Min(a, b).Rank() * 10 + Math.Max(a, b).Rank(); "
        "  end Main; "
        "end C; "), 37);
}

// CONTROL — the integer path, which never consults the comparator at all.
// Plain runAot: nothing is allocated, so ASan has nothing to say.
BEZEL_TEST(math_minmax_iface_arg_2782, control_int_argument_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    return Math.Min(7, 3) * 10 + Math.Max(7, 3); "
        "  end Main; "
        "end C; "), 37);
}
