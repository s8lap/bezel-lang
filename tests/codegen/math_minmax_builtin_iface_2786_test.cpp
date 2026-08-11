// Regression tests for #2786 — `Math.Min` / `Math.Max` / `Math.Clamp` with a
// BUILTIN interface argument (`IDisposable`), and more generally the fail-closed
// guard in front of the float fallback.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §2.2 ("`Math`", v0.240) declares
//     public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
// `IDisposable` (stdlib §2.5) declares no `CompareTo` and names no
// `IComparable`, so it does NOT satisfy that constraint and the call must be
// REJECTED with a diagnostic — exactly as a user-defined non-comparable
// interface already was.  Instead it ABORTED the compiler:
//     SIToFP source must be integer or integer vector
// because the operand fell past every arm into the float fallback, which does
// `CreateSIToFP` on what is here a pointer.
//
// WHY THE GUARD IS CATEGORY-BLIND.  It tests the LOWERED LLVM type rather than
// enumerating source categories, because enumerating is what produced this
// entire family: the kind test listed Record|Enum|Class and was then found to be
// missing Interface (#2782), `string` (#2785), builtin interfaces (this issue),
// and `decimal` (#2788).  A guard that must be extended for each newly
// discovered category is a guard that will be found missing a fifth.
//
// ROOT CAUSE, recorded but NOT fixed here.  The whitelist at
// TypeResolution_Names.cpp:1511 attaches a real synthesized Interface TypeSymbol
// to `IComparable`/`IEquatable`/`IEqualityComparer`/`IReadOnlySpan`/`INumber`/
// `ISteppable`/`IParsable` — and NOT to `IDisposable`, whose TypeRefs therefore
// carry no `declaration` at all.  Every gate in this family keys on
// `declaration`, which is why the same blind spot appears as #2784 (the §5.1
// gate bypassed for `Array<IDisposable>`) and #2766 (no `resolvedDeclType`, so a
// method-group thunk fell back to an i32 placeholder).  Adding IDisposable to
// that whitelist would close all three at the source, but it changes the symbol
// representation of the type the whole §28 disposal machinery is built on — out
// of scope for a bug fix.
//
// ⚠ NOT PINNED HERE ON PURPOSE: that `decimal` arguments are rejected.  They
// are, as of this guard (they aborted before it — measured), but that is a
// STOPGAP, not the required behaviour: stdlib §4.4 gives decimal a total order
// and #2788 tracks making the call actually work.  Pinning the rejection would
// make the correct fix look like a regression.

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
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
// Diagnostic TEXT, not the coarse reject: the defect under test WAS a compiler
// abort, and runAot synthesizes an Error for that too — so "did not run" alone
// cannot distinguish the fix from the defect.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

const char* kNode =
    "module M; "
    "class Node "
    "  public constructor() end constructor; "
    "end Node; ";

std::string disposableMain(const std::string& call) {
    return std::string(kNode) +
           "class C  public static method Main(): int "
           "    using var a := new Node(); "
           "    using var b := new Node(); "
           "    using var c := new Node(); "
           "    var x: IDisposable := a; "
           "    var y: IDisposable := b; "
           "    var z: IDisposable := c; " +
           call +
           "    return 7; "
           "  end Main; end C; ";
}
const char* kNeedle = "could not lower a comparison for this argument type";
}  // namespace

// #2786 / stdlib §2.2 + §2.5 — the reported repro.  ABORTED the compiler before
// this guard; now a spec-cited diagnostic.
BEZEL_TEST(math_minmax_builtin_iface_2786, min_builtin_interface_rejected) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(disposableMain("    var t := Math.Min(x, y); "), kNeedle));
}

BEZEL_TEST(math_minmax_builtin_iface_2786, max_builtin_interface_rejected) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(disposableMain("    var t := Math.Max(x, y); "), kNeedle));
}

// `Clamp` reaches the same fallback from its own call site — all three are
// guarded, so a later change that guards only Min/Max is caught here.
BEZEL_TEST(math_minmax_builtin_iface_2786, clamp_builtin_interface_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        disposableMain("    var t := Math.Clamp(x, y, z); "), kNeedle));
}

// CONTROLS — every category that DOES lower must be untouched by a guard that
// sits in front of the float fallback.  These are the four arms tried before it.

BEZEL_TEST(math_minmax_builtin_iface_2786, control_int_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C  public static method Main(): int "
        "    return Math.Min(7, 3) * 10 + Math.Max(7, 3); "
        "  end Main; end C; "), 37);
}

BEZEL_TEST(math_minmax_builtin_iface_2786, control_double_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C  public static method Main(): int "
        "    var a: double := 2.5; "
        "    var b: double := 1.5; "
        "    var m := Math.Min(a, b); "
        "    if m < 2.0 then return 15; end if; "
        "    return 25; "
        "  end Main; end C; "), 15);
}

// `char` lowers to an integer, so it takes the integer arm and never reaches
// the guard — pinned because a guard keyed on "not a pointer" rather than on
// the lowered numeric type could plausibly have caught it.
BEZEL_TEST(math_minmax_builtin_iface_2786, control_char_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C  public static method Main(): int "
        "    var a: char := 'z'; "
        "    var b: char := 'a'; "
        "    var m := Math.Min(a, b); "
        "    if m = 'a' then return 73; end if; "
        "    return 74; "
        "  end Main; end C; "), 73);
}

// A CLASS implementing IComparable — the #1148 arm, which resolves a real
// CompareTo and returns before the fallback.
BEZEL_TEST(math_minmax_builtin_iface_2786, control_comparable_class_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box: IComparable<Box> "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Value(): int => _v; "
        "  public method CompareTo(other: Box): int => _v - other.Value(); "
        "end Box; "
        "class C  public static method Main(): int "
        "    using var a := new Box(7); "
        "    using var b := new Box(3); "
        "    return Math.Min(a, b).Value() * 10 + Math.Max(a, b).Value(); "
        "  end Main; end C; "), 37);
}

// A `string` argument — the #2785 arm, likewise before the fallback.
BEZEL_TEST(math_minmax_builtin_iface_2786, control_string_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C  public static method Main(): int "
        "    var p: string := \"pear\"; "
        "    var q: string := \"fig\"; "
        "    return Math.Min(p, q).Length * 10 + Math.Max(p, q).Length; "
        "  end Main; end C; "), 34);
}
