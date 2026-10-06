// Regression tests for #2772 item 2 — the class arm of the `Array<T>.Sort()`
// IComparable gate ignores the type argument in `IComparable<TypeArg>`.
//
// SPEC SOURCE.  stdlib §4.4 (v0.276, the governing document at filing)
// declares `Sort()` only inside `where T: IComparable<T>`, so the admissibility
// condition is exactly `T: IComparable<T>` — spec §14.2's conformance rule, with
// the self-recurrence: T must conform to `IComparable<T>`, not `IComparable<Any>`.
//
// THE DEFECT.  The class arm checked only that the class DECLARES IComparable,
// by name, using `declaresBuiltinInterface` — which confirms the interface is
// present but not that the type argument is Self.  A class `X: IComparable<Other>`
// satisfied the gate even though X does NOT implement `IComparable<X>`; codegen then
// found `X.CompareTo(other: Other)` via `findMethodByName` and called it through
// the `bz$classcmp$X` thunk with two `X` pointers where the declared parameter is
// `Other` — a type-confused call, not merely a wrong ordering.
//
// THE FIX.  The class arm now iterates the class's `baseTypeRefs` and
// SUBSTITUTES the element type's generic arguments (e.g., T -> int for
// Box<int>) before comparing the IComparable type argument against the
// concrete element type.  This correctly handles both non-generic classes
// (where the base type argument is already concrete) and generic classes
// (where the base type argument contains unsubstituted generic parameters).
//
// ROOT CAUSE WAS CONFIRMED MECHANICALLY on r23-c28: `class X: IComparable<Other>`
// compiled to exit 0 then SEGV'd at runtime.  After the fix it is rejected at
// compile time with the standard gate diagnostic.

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
// Diagnostic TEXT: these shapes previously COMPILED, so "did not run" alone
// would also be satisfied by an unrelated crash.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A class that implements IComparable<Other>, NOT IComparable<Self>.  This is
// the exact shape the bug admitted: declares IComparable (satisfies the
// name-only check) but with the wrong type argument.
const char* kWrongArg =
    "module M; "
    "class Other "
    "  public constructor() end constructor; "
    "end Other; "
    "class X: IComparable<Other> "
    "  public constructor() end constructor; "
    "  public method CompareTo(other: Other): int => 0; "
    "end X; ";

// A class that implements IComparable<Self> correctly.
const char* kCorrectArg =
    "module M; "
    "class Box: IComparable<Box> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method V(): int => _v; "
    "  public method CompareTo(other: Box): int => _v - other.V(); "
    "end Box; ";

// GENERIC CLASS FIXTURES — the same three shapes as above but with a type
// parameter.  These test that the IComparable<T> gate applies to GENERIC
// classes correctly (the previous fix admitted ALL generic classes
// unconditionally, which was a soundness hole).
//
// A generic class that implements IComparable<Other>, NOT IComparable<Self>.
// The wrong type argument means Box<int> does NOT implement IComparable<Box<int>>.
const char* kGenericWrongArg =
    "module M; "
    "class Other "
    "  public constructor() end constructor; "
    "end Other; "
    "class Box<T>: IComparable<Other> "
    "  private _v: T; "
    "  public constructor(v: T) _v := v; end constructor; "
    "  public method V(): T => _v; "
    "  public method CompareTo(other: Other): int => 0; "
    "end Box; ";

// A generic class with no IComparable conformance at all.
const char* kGenericNoIComparable =
    "module M; "
    "class Box<T> "
    "  private _v: T; "
    "  public constructor(v: T) _v := v; end constructor; "
    "  public method V(): T => _v; "
    "end Box; ";

std::string main_(const char* preamble, const std::string& body) {
    return std::string(preamble) +
           "class C  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #2772 item 2 / spec §14.2 — THE REGRESSION.  Before the fix, a class
// implementing IComparable<Other> was admitted for Array<X>.Sort() even though
// X does not implement IComparable<X>.  Codegen found X.CompareTo(Other) and
// called it with two X pointers — type confusion, confirmed by a runtime
// SIGSEGV before the fix.  After the fix this is rejected with the standard
// gate diagnostic, naming the substituted element type and the structural
// reason.
BEZEL_TEST(array_sort_wrong_icomp_arg_2772,
           sort_class_wrong_icomp_type_arg_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kWrongArg,
        "    using var a := new X(); "
        "    using var b := new X(); "
        "    var arr: Array<X> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "),
        "Array<X>.Sort() requires T: IComparable<T>"));
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kWrongArg,
        "    using var a := new X(); "
        "    using var b := new X(); "
        "    var arr: Array<X> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "),
        "X does not implement IComparable<X>"));
}

// CONTROL — a class that DOES implement IComparable<Self> must still sort
// correctly.  3,1,2 sorted ascending is 1,2,3: 1*16 + 2*4 + 3 = 27.
BEZEL_TEST(array_sort_wrong_icomp_arg_2772,
           control_correct_icomp_self_still_sorts) {
    BEZEL_EXPECT_EQ(runChecked(main_(kCorrectArg,
        "    using var a := new Box(3); "
        "    using var b := new Box(1); "
        "    using var c := new Box(2); "
        "    var arr: Array<Box> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].V() * 16 + arr[1].V() * 4 + arr[2].V(); ")), 27);
}

// CONTROL — the explicit-comparer form remains admissible for the wrong-arg
// class.  Supplying a comparer bypasses the IComparable gate entirely and is
// the correct way to sort such a type.
BEZEL_TEST(array_sort_wrong_icomp_arg_2772,
           control_wrong_arg_with_explicit_comparer_accepted) {
    BEZEL_EXPECT_EQ(runChecked(main_(kWrongArg,
        "    using var a := new X(); "
        "    using var b := new X(); "
        "    var arr: Array<X> := [a, b]; "
        "    arr.Sort(lambda(x: X, y: X) => 0); "
        "    return 7; ")), 7);
}

// CONTROL — the same wrong-arg class with OwningArray.Sort() is also rejected
// (the gate covers both Array and OwningArray per #3325).
BEZEL_TEST(array_sort_wrong_icomp_arg_2772,
           owning_sort_class_wrong_icomp_type_arg_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kWrongArg,
        "    using var arr := new OwningArray<X>(); "
        "    arr.Add(new X()); arr.Add(new X()); "
        "    arr.Sort(); "
        "    return 7; "),
        "OwningArray<X>.Sort() requires T: IComparable<T>"));
}

// GENERIC CLASS REGRESSION TESTS.  The original fix (#2772 item 2) admitted
// ALL generic classes unconditionally in the class arm of the IComparable gate,
// because comparing the unsubstituted base type argument (Box<T>) against the
// instantiated element type (Box<int>) failed for the wrong reason (T vs int).
// The unconditional admit was a soundness hole: Box<T>: IComparable<Other>
// compiled and SEGV'd at runtime, and Box<T> (no IComparable) compiled and
// SEGV'd at runtime.  The substitution-based check closes both holes.

// GENERIC REGRESSION — Box<T>: IComparable<Other> with Array<Box<int>>.Sort()
// is now correctly rejected.  Before the fix: compiled, wrong sort result.
// After the fix: rejected with the standard gate diagnostic naming Box<int>
// and the IComparable<Box<int>> self-conformance requirement.
BEZEL_TEST(array_sort_wrong_icomp_arg_2772,
           generic_wrong_icomp_type_arg_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kGenericWrongArg,
        "    using var a := new Box<int>(3); "
        "    using var b := new Box<int>(1); "
        "    var arr: Array<Box<int>> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "),
        "Array<Box<int>>.Sort() requires T: IComparable<T>"));
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kGenericWrongArg,
        "    using var a := new Box<int>(3); "
        "    using var b := new Box<int>(1); "
        "    var arr: Array<Box<int>> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "),
        "Box<int> does not implement IComparable<Box<int>>"));
}

// GENERIC REGRESSION — Box<T> with no IComparable at all, with
// Array<Box<int>>.Sort(), is now correctly rejected.  Before the fix:
// compiled and SEGV'd at runtime (codegen found no CompareTo and called
// through a null or garbage function pointer).  After the fix: rejected
// with the standard gate diagnostic.
BEZEL_TEST(array_sort_wrong_icomp_arg_2772,
           generic_no_icomp_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kGenericNoIComparable,
        "    using var a := new Box<int>(3); "
        "    using var b := new Box<int>(1); "
        "    var arr: Array<Box<int>> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "),
        "Array<Box<int>>.Sort() requires T: IComparable<T>"));
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kGenericNoIComparable,
        "    using var a := new Box<int>(3); "
        "    using var b := new Box<int>(1); "
        "    var arr: Array<Box<int>> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; "),
        "Box<int> does not implement IComparable<Box<int>>"));
}

// GENERIC CONTROL — a generic class that DOES implement IComparable<Box<T>>
// correctly must still sort correctly.  3,1,2 sorted ascending is 1,2,3:
// 1*16 + 2*4 + 3 = 27.  CompareTo is implemented as _v - other.V() so the
// ordering is correct and the return-value check verifies it.
BEZEL_TEST(array_sort_wrong_icomp_arg_2772,
           generic_correct_icomp_self_sorts) {
    // Inline the class here to give it a working CompareTo.
    const char* program =
        "module M; "
        "class Box<T>: IComparable<Box<T>> "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method V(): int => _v; "
        "  public method CompareTo(other: Box<T>): int => _v - other.V(); "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box<int>(3); "
        "    using var b := new Box<int>(1); "
        "    using var c := new Box<int>(2); "
        "    var arr: Array<Box<int>> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].V() * 16 + arr[1].V() * 4 + arr[2].V(); "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(runChecked(std::string(program)), 27);
}
