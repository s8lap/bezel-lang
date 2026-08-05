// Regression tests for #2784 — `Array<IDisposable>.Min()` / `.Max()` compiled
// with NO diagnostic at all and returned a non-null element chosen under no
// defined ordering.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §5.1 ("No-selector aggregation
// overloads", v0.240) declares these forms only inside
//     public extension IEnumerable<T> where T: IComparable<T>
// and `IDisposable` (stdlib §2.5) declares no `CompareTo` and names no
// `IComparable`, so the constraint is unsatisfied and the call must be
// REJECTED — exactly as the user-defined analogue already was.
//
// WHY BOTH GATES MISSED IT.  Sema's #1198 gate scans the ELEMENT DECLARATION's
// base list for `IComparable`, and codegen's #2770 reject guard required
// `elemTypeRef->declaration` before firing.  A BUILTIN interface has no
// declaration to scan or to test: the whitelist at
// TypeResolution_Names.cpp:1511 attaches a synthesized Interface TypeSymbol to
// IComparable/IEquatable/IEqualityComparer/IReadOnlySpan/INumber/ISteppable/
// IParsable and NOT to IDisposable (root cause, filed as #2789 — it is also the
// shared root of #2786 and #2766).  So the call fell through every gate.
//
// THE FIX drops the declaration requirement from the codegen guard: what makes
// an element admissible is that a comparator was RESOLVED by the arms above,
// not that it happens to carry a declaration.  Same reasoning as #2786's
// category-blind guard — enumerating the shapes that may arrive is how this
// family kept acquiring new members.
//
// MEASURED BLAST RADIUS (not inferred from the diff).  The only Named TypeRefs
// without a declaration are builtins not exported by a dependency — TypeRef.hpp
// states this, and it was checked against the case that would matter most: a
// CROSS-MODULE comparable interface element, built as two real modules
// (`bezelc -c lib.bzl` then `bezelc --import lib.o app.bzl`).  That shape is
// rejected BOTH before and after this change — the imported interface does
// carry a declaration, so the old guard already fired on it.  It is #2776's
// separately-recorded limit (an imported interface's `baseTypeRefs` does not
// carry its `IComparable` base), untouched here.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
// Diagnostic TEXT: the defect was SILENT ACCEPTANCE, so "did not run" alone
// would also be satisfied by an unrelated crash.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
const char* kNeedle = "requires the element type to satisfy";

std::string disposableArray(const char* op) {
    return std::string(
        "module M; "
        "class Node "
        "  public constructor() end constructor; "
        "  public method Tag(): int => 4; "
        "end Node; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(); "
        "    using var b := new Node(); "
        "    var arr: Array<IDisposable> := [a, b]; "
        "    var m := arr.") + op + "(); "
        "    if m = null then return 91; end if; "
        "    return 92; "
        "  end Main; "
        "end C; ";
}
}  // namespace

// #2784 / stdlib §5.1 + §2.5 — the reported repro.  Before the fix this
// compiled with NO diagnostic and exited 92, i.e. `Min()` produced a non-null
// pointer under no defined ordering.
BEZEL_TEST(array_min_builtin_iface_elem_2784, min_builtin_interface_element_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(disposableArray("Min"), kNeedle));
}

BEZEL_TEST(array_min_builtin_iface_elem_2784, max_builtin_interface_element_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(disposableArray("Max"), kNeedle));
}

// CONTROL — a USER-DEFINED interface element that DOES satisfy the §5.1
// constraint still works.  This is the case the widened guard would break if it
// had been widened one step too far (to "any interface element"), so it is the
// most important control in the file.  Ranks 7 and 3, ascending CompareTo:
// Min*10 + Max = 37.
BEZEL_TEST(array_min_builtin_iface_elem_2784, control_self_comparable_interface_still_works) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "public interface IFoo: IComparable<IFoo> "
        "  method Rank(): int; "
        "end IFoo; "
        "class Node: IFoo "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IFoo): int => _r - other.Rank(); "
        "end Node; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(7); "
        "    using var b := new Node(3); "
        "    var arr: Array<IFoo> := [a, b]; "
        "    return arr.Min().Rank() * 10 + arr.Max().Rank(); "
        "  end Main; "
        "end C; "), 37);
}

// CONTROL — a CLASS element with a real CompareTo, the arm that resolves before
// the guard is ever reached.
BEZEL_TEST(array_min_builtin_iface_elem_2784, control_comparable_class_element_still_works) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box: IComparable<Box> "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Value(): int => _v; "
        "  public method CompareTo(other: Box): int => _v - other.Value(); "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box(7); "
        "    using var b := new Box(3); "
        "    var arr: Array<Box> := [a, b]; "
        "    return arr.Min().Value() * 10 + arr.Max().Value(); "
        "  end Main; "
        "end C; "), 37);
}

// CONTROL — the primitive element path, which never consults the element
// declaration at all.
BEZEL_TEST(array_min_builtin_iface_elem_2784, control_int_element_still_works) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var arr: Array<int> := [3, 1, 2]; "
        "    return arr.Min() * 10 + arr.Max(); "
        "  end Main; "
        "end C; "), 13);
}
