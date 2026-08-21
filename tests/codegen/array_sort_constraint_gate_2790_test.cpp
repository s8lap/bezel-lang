// Regression tests for #2790 — the PARAMETERLESS `Array<T>.Sort()` had no
// admissibility gate at all, while its sibling `Sorted()` did.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §4.4 (v0.240) constrains both forms
// identically — `where T: IComparable<T>` — so a `T` that implements no
// comparison must be REJECTED whichever form is written.
//
// THE DEFECT WAS A SILENT NO-OP, and on an ordinary mistake rather than an
// exotic one.  `Array<Plain>.Sort()` for a `Plain` implementing no `IComparable`
// COMPILED, RAN, and DID NOTHING: elements 3,1,2 came back 3,1,2.  The user asks
// for a sort and gets an unsorted array with no diagnostic.  `Sorted()` on the
// very same type was correctly rejected.
//
// ROOT CAUSE was simpler than the issue first guessed (it hypothesised the loose
// class branch recorded on #2772).  The gate's branch was entered under
// `m == "Sorted" && ce.args.empty()`, so `Sort()` never reached it.  Both forms
// now share the branch; only the RESULT TYPE differs — `Sorted()` yields a new
// Array, `Sort()` sorts in place and yields no value — which is why they cannot
// simply collapse into one arm.
//
// ⚠ MEASURED WITHOUT THE 8-BIT TRAP.  The obvious encoding
// (`arr[0]*100 + arr[1]*10 + arr[2]`) gives 312 for the unsorted order, which
// `WEXITSTATUS` truncates to 56 — a value that looks like nothing in particular
// and hides what happened.  These cases use base-4 (`*16, *4, *1`): 3,1,2 → 54
// and 1,2,3 → 27, both far below 256, so no truncation is involved.
//
// This fix also CLOSED the divergence recorded by #2787's cross-gate agreement
// test: `Array<T>.Sort()`, `Array<T>.Min()` and `Math.Min` now give the same
// answer for the same element type. Four test cases elsewhere that pinned the
// old accepting behaviour were updated with explanations — three #2768 controls
// and #2787's `gate_sort_diverges_pinned_2790`, which was written to fail on
// exactly this fix.

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
// Diagnostic TEXT: these shapes previously COMPILED, so "did not run" alone
// would also be satisfied by an unrelated crash.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

const char* kPlain =
    "module M; "
    "class Plain "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method V(): int => _v; "
    "end Plain; ";

const char* kBox =
    "module M; "
    "class Box: IComparable<Box> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method V(): int => _v; "
    "  public method CompareTo(other: Box): int => _v - other.V(); "
    "end Box; ";

std::string main_(const char* preamble, const std::string& body) {
    return std::string(preamble) +
           "class C  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #2790 / stdlib §4.4 — the repro.  Before the fix this compiled and silently
// left the array unsorted.
// D2 contract needles (per #3329 / #3325): the diagnostic spells the element
// name (`Plain`) in both the constraint and the comparer substitution, not
// the bare `T`.
BEZEL_TEST(array_sort_constraint_gate_2790, sort_non_comparable_class_rejected) {
    // ⚠ UPDATED BY #3329 / proposal 0116 §3 D2 (spec §14.2, stdlib §4.4,
    // v0.276).  The pre-fix needle was `"Array<T>.Sort() requires T to
    // implement"` — that text never matched the substituted form
    // (`Array<Plain>`) and quoted the retired stdlib §3 via the message
    // body.  The current needle pin the FOUR parts of D2: the substituted
    // receiver (`Array<Plain>`), the §14.2 condition wording, the
    // structural reason (`Plain does not implement IComparable<Plain>`),
    // and the form the user wrote.
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kPlain,
        "    using var a := new Plain(3); "
        "    using var b := new Plain(1); "
        "    using var c := new Plain(2); "
        "    var arr: Array<Plain> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].V() * 16 + arr[1].V() * 4 + arr[2].V(); "),
"Array<Plain>.Sort() requires T: IComparable<T> "
        "(spec §14.2, stdlib §4.4, v0.276); Plain does not implement "
        "IComparable<Plain>"));
}

// The diagnostic must name the form the USER WROTE and point at THAT form's
// comparer overload.  Telling someone who called `Sort()` to use
// `Sorted(comparer)` sends them to a different operation — a copy, not an
// in-place sort — so the message is parameterised rather than hardcoded.
// ⚠ UPDATED BY #3329: the pre-fix needle was
// `"Use Sort(comparer: (T, T) -> int) instead"` — the substituted type
// name `Plain` now appears in the parenthetical.
BEZEL_TEST(array_sort_constraint_gate_2790, diagnostic_names_the_form_written) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kPlain,
        "    using var a := new Plain(3); "
        "    using var b := new Plain(1); "
        "    var arr: Array<Plain> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "),
        "Use Sort(comparer: (Plain, Plain) -> int) instead"));
}

// ...and `Sorted()` keeps naming ITSELF — the shared gate must not have made
// both forms report one name. ⚠ UPDATED BY #3329: the substituted type
// `Plain` appears in the remed overload's parenthetical.
BEZEL_TEST(array_sort_constraint_gate_2790, sorted_still_names_itself) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kPlain,
        "    using var a := new Plain(3); "
        "    using var b := new Plain(1); "
        "    var arr: Array<Plain> := [a, b]; "
        "    var s := arr.Sorted(); "
        "    return 7; "),
        "Use Sorted(comparer: (Plain, Plain) -> int) instead"));
}

// #3329 / proposal 0116 §3 D2 (spec §14.2, stdlib §4.4, v0.276):
// the FIRST of the diagnostic's three defects is that the receiver
// type is printed UNSUBSTITUTED — `Array<T>.Sort()` tells the user the
// rule but not which of their types broke it.  This test pins the
// substituted form: `Array<Widget>` for a receiver on the user-named
// class `Widget`.  Without the fix the substring would not match; the
// existing #2790 tests are coarse enough that the fix could regress
// item 1 of D2 without their noticing.
BEZEL_TEST(array_sort_constraint_gate_2790,
           diagnostic_names_substituted_type_array_3329) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "  public method Tag(): int => 1; "
        "end Widget; "
        "class C  public static method Main(): int "
        "  using var a := new Widget(); "
        "  var arr: Array<Widget> := [a]; "
        "  arr.Sort(); "
        "  return 7; "
        "end Main; end C; ",
        "Array<Widget>.Sort()"));
}

// #3329 — the STRUCTURAL reason must name the substituted type in
// the constraint position, not "T is not a built-in comparable type".
// The pre-fix text printed an accept-list (the implementation's
// internal shortlist of primitives and class types), which goes stale
// every time the list grows and never names the remedy.  The current
// form names the substituted type and the constraint it fails to
// satisfy.
BEZEL_TEST(array_sort_constraint_gate_2790,
           diagnostic_states_structural_reason_3329) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "  public method Tag(): int => 1; "
        "end Widget; "
        "class C  public static method Main(): int "
        "  using var a := new Widget(); "
        "  var arr: Array<Widget> := [a]; "
        "  arr.Sort(); "
        "  return 7; "
        "end Main; end C; ",
        "Widget does not implement IComparable<Widget>"));
}

// #3329 — the diagnostic must NOT cite the retired stdlib §3.  The
// live homes are stdlib §4.4 (the declarations and the condition) and
// spec §14.2 (the constraint vocabulary).  A diagnosis that quotes
// retired §3 sends the reader to a nonexistent section.
BEZEL_TEST(array_sort_constraint_gate_2790,
           diagnostic_cites_current_stdlib_section_3329) {
    auto r = runAot(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "  public method Tag(): int => 1; "
        "end Widget; "
        "class C  public static method Main(): int "
        "  using var a := new Widget(); "
        "  var arr: Array<Widget> := [a]; "
        "  arr.Sort(); "
        "  return 7; "
        "end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool sawCurrent = false;
    bool sawRetired = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("stdlib §4.4") != std::string::npos)
            sawCurrent = true;
        if (d.message.find("§3 / #472") != std::string::npos)
            sawRetired = true;
    }
    BEZEL_EXPECT_TRUE(sawCurrent);
    BEZEL_EXPECT_FALSE(sawRetired);
}

// CONTROL — a class that DOES implement IComparable<T> must still sort in place.
// This is the case an over-wide gate would break, so it is the most important
// control in the file.  3,1,2 sorted ascending is 1,2,3 → 1*16 + 2*4 + 3 = 27.
BEZEL_TEST(array_sort_constraint_gate_2790, control_comparable_class_still_sorts) {
    BEZEL_EXPECT_EQ(runChecked(main_(kBox,
        "    using var a := new Box(3); "
        "    using var b := new Box(1); "
        "    using var c := new Box(2); "
        "    var arr: Array<Box> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].V() * 16 + arr[1].V() * 4 + arr[2].V(); ")), 27);
}

// CONTROL — the primitive element path, which the gate accepts via its own
// branch and which never consults a declaration.
BEZEL_TEST(array_sort_constraint_gate_2790, control_int_elements_still_sort) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C  public static method Main(): int "
        "    var arr: Array<int> := [3, 1, 2]; "
        "    arr.Sort(); "
        "    return arr[0] * 16 + arr[1] * 4 + arr[2]; "
        "  end Main; end C; "), 27);
}

// CONTROL — `string` elements, likewise a built-in comparable.  "c","a","b"
// sorted ordinally is "a","b","c"; the first element's length is 1 either way,
// so compare content: 1 if sorted, 2 otherwise.
// Plain runAot, not runChecked, for the INDEPENDENT pre-existing `Array<string>`
// collection-literal leak recorded on #2774 — LeakSanitizer overrides the
// process exit code and would mask the value under test.  Same accommodation,
// for the same shape and reason, as in the #2770 and #2785 suites.
BEZEL_TEST(array_sort_constraint_gate_2790, control_string_elements_still_sort) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C  public static method Main(): int "
        "    var arr: Array<string> := [\"c\", \"a\", \"b\"]; "
        "    arr.Sort(); "
        "    if arr[0] = \"a\" and arr[2] = \"c\" then return 1; end if; "
        "    return 2; "
        "  end Main; end C; "), 1);
}

// CONTROL — the explicit-comparer form takes a different branch entirely and
// must remain admissible for a NON-comparable element: supplying the comparer is
// exactly how a user sorts such a type.  An over-wide gate that rejected on the
// element type alone would break this, which is the whole point of the overload.
BEZEL_TEST(array_sort_constraint_gate_2790, control_explicit_comparer_still_admitted) {
    BEZEL_EXPECT_EQ(runChecked(main_(kPlain,
        "    using var a := new Plain(3); "
        "    using var b := new Plain(1); "
        "    using var c := new Plain(2); "
        "    var arr: Array<Plain> := [a, b, c]; "
        "    arr.Sort(lambda(x: Plain, y: Plain) => x.V() - y.V()); "
        "    return arr[0].V() * 16 + arr[1].V() * 4 + arr[2].V(); ")), 27);
}

// CONTROL — a self-comparable INTERFACE element (#2768/#2770's shape) still
// sorts through the itable.  The gate must decline non-conforming interfaces
// without taking this one with it.
BEZEL_TEST(array_sort_constraint_gate_2790, control_self_comparable_interface_still_sorts) {
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
        "class C  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    using var c := new Node(2); "
        "    var arr: Array<IFoo> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].Rank() * 16 + arr[1].Rank() * 4 + arr[2].Rank(); "
        "  end Main; end C; "), 27);
}
