// Regression tests for #3325 — `OwningArray<T>.Sort()` was admitted with no
// `IComparable<T>` gate, mirroring #2790's `Array<T>.Sort()` defect which was
// fixed only on the borrowing side.  The owning side, separately declared in
// stdlib §4.5, was left with the same gap: three sema sites positively admit
// it (the §22.5 owning-container allow-list names `Sort`; the result-typing
// arm names `OwningArray` explicitly; the sole rejection arm covers
// `Reversed`/`Sorted` and omits `Sort`), and codegen reached `Sort` because
// `OwningArray` is array-backed.  With no `CompareTo` and no interface
// comparator, the codegen arm selected `bz$strcmp` on each element pointed
// at as a `char*` — and for a class element without `CompareTo`, that is the
// vtable pointer of the element (the same unbounded read that #2768's
// `Array<T>.Sort()` and `Sorted()` were fixed to avoid).
//
// Spec source for the gate: stdlib §4.4 and §4.5 (v0.276) state that
// `Sort()` (`Sort()` not `Sort(comparer)`) requires `T: IComparable<T>` —
// the same constraint on both `Array<T>` and `OwningArray<T>`.  Spec §14.2
// (v0.275) and language spec §3 spell the comparator-element requirement.
// The fix widens the sema gate at ExprTypecheck_Calls.cpp:5521 to include
// `OwningArray`, with the existing `Sorted`/`Reversed` `OwningArray<T>`
// rejection (line 5694) kept as the authority for those copy-forms.
//
// These tests are the owning-side mirror of #2790's suite: failing ones
// pin the bug (the class-without-CompareTo case), controls confirm the
// gate did not over-fire (comparable class, int, string, explicit comparer
// on a non-comparable class, self-comparable interface).

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

// #2790's kPlain / kBox mirrored onto the owning side.  `OwningArray<T>`
// requires `T: class` (stdlib §4.5), so we cannot use a record here; both
// fixtures are classes.
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

// #3325 / stdlib §4.5 / spec §14.2 — the repro.  Before the fix this
// COMPILED, the program RAN, and the order was silently wrong: strcmp on
// the vtable pointers of the three Plain instances is NOT the user's
// natural-order CompareTo (they have none), so the order is in practice
// whatever the allocator handed out.  The expected fix REJECTS this
// program with a diagnostic naming `IComparable<T>`, mirroring #2790's
// `Array<T>.Sort()` gate.
//
// The diagnostic must name the form the USER WROTE and the receiver — so
// it reads `OwningArray<Plain>.Sort() requires T: IComparable<T> (...)`
// (the receiver is owning, not borrowing, AND the element name `Plain`
// is spelled out, not the bare `T`), and points at THAT form's comparer
// overload — `Sort(comparer: (Plain, Plain) -> int)`, not `Sorted(...)`.
// This is the four-part D2 contract format that #3329 set for the gate
// and #3325 (W1B on top of W1D) extended to also parameterize the
// stdlib section (§4.5 for `OwningArray`, §4.4 for `Array`).
//
// Note: `arr.Add(item: T)` on `OwningArray<T>` takes ownership (stdlib
// §4.5) so the test passes fresh `new Plain(...)` directly — there is
// no `using var` indirection through which to read the per-element value
// after the call has consumed it, and that is by design (the array
// owns the elements now).
BEZEL_TEST(owningarray_sort_constraint_gate_3325, sort_non_comparable_class_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kPlain,
        "    using var arr := new OwningArray<Plain>(); "
        "    arr.Add(new Plain(3)); arr.Add(new Plain(1)); arr.Add(new Plain(2)); "
        "    arr.Sort(); "
        "    return arr.Get(0).V() * 16 + arr.Get(1).V() * 4 + arr.Get(2).V(); "),
        "OwningArray<Plain>.Sort() requires T: IComparable<T>"));
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kPlain,
        "    using var arr := new OwningArray<Plain>(); "
        "    arr.Add(new Plain(3)); arr.Add(new Plain(1)); arr.Add(new Plain(2)); "
        "    arr.Sort(); "
        "    return arr.Get(0).V() * 16 + arr.Get(1).V() * 4 + arr.Get(2).V(); "),
        "Plain does not implement IComparable<Plain>"));
}

// The diagnostic must name the form the USER WROTE and point at THAT form's
// comparer overload, with the element name spelled out (per the D2 contract).
// Telling someone who called `Sort()` to use `Sorted(comparer)` sends them to
// a different operation — a copy, not an in-place sort — so the message is
// parameterised rather than hardcoded.
BEZEL_TEST(owningarray_sort_constraint_gate_3325, diagnostic_names_the_form_written) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kPlain,
        "    using var arr := new OwningArray<Plain>(); "
        "    arr.Add(new Plain(3)); arr.Add(new Plain(1)); "
        "    arr.Sort(); "
        "    return 7; "),
        "Use Sort(comparer: (Plain, Plain) -> int) instead"));
}

// `Sorted()` is NOT defined on `OwningArray<T>` (stdlib §4.5 — copy-forms
// are absent on the owning side because a shallow copy would double-own
// each element, violating §28.3).  The pre-existing rejection at
// ExprTypecheck_Calls.cpp:5730 must STILL be the authority for `Sorted()`
// — the new Sort gate must not have stolen that diagnosis, and the
// diagnostic must NOT mention `IComparable<T>` (the reason is ownership,
// not element comparability).
BEZEL_TEST(owningarray_sort_constraint_gate_3325, sorted_is_still_not_defined_on_owning) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(kBox,
        "    using var arr := new OwningArray<Box>(); "
        "    arr.Add(new Box(3)); arr.Add(new Box(1)); "
        "    var s := arr.Sorted(); "
        "    return 7; "),
        "Sorted() is not defined on OwningArray<T>"));
}

// CONTROL — a class that DOES implement IComparable<T> must still sort in
// place.  This is the case an over-wide gate would break, so it is the
// most important control in the file.  3,1,2 sorted ascending is 1,2,3:
// 1*16 + 2*4 + 3 = 27.
BEZEL_TEST(owningarray_sort_constraint_gate_3325, control_comparable_class_still_sorts) {
    BEZEL_EXPECT_EQ(runChecked(main_(kBox,
        "    using var arr := new OwningArray<Box>(); "
        "    arr.Add(new Box(3)); arr.Add(new Box(1)); arr.Add(new Box(2)); "
        "    arr.Sort(); "
        "    return arr.Get(0).V() * 16 + arr.Get(1).V() * 4 + arr.Get(2).V(); ")), 27);
}

// CONTROL — the explicit-comparer form takes a different branch entirely
// and must remain admissible for a NON-comparable element: supplying the
// comparer is exactly how a user sorts such a type.  An over-wide gate
// that rejected on the element type alone would break this, which is the
// whole point of the `Sort(comparer)` overload on `OwningArray<T>`.
BEZEL_TEST(owningarray_sort_constraint_gate_3325, control_explicit_comparer_still_admitted) {
    BEZEL_EXPECT_EQ(runChecked(main_(kPlain,
        "    using var arr := new OwningArray<Plain>(); "
        "    arr.Add(new Plain(3)); arr.Add(new Plain(1)); arr.Add(new Plain(2)); "
        "    arr.Sort(lambda(x: Plain, y: Plain) => x.V() - y.V()); "
        "    return arr.Get(0).V() * 16 + arr.Get(1).V() * 4 + arr.Get(2).V(); ")), 27);
}

// CONTROL — a self-comparable INTERFACE element (#2768/#2770's shape)
// still sorts through the §13 itable.  `OwningArray<T>` requires `T: class`,
// so the IComparable<IFoo> interface-substitution is the exact mirror of
// the #2790 control on the borrowing side, holding the gate to the
// §14.2 interface-conformance rule 3 self-recurrence and not over-firing.
// 3,1,2 sorted ascending is 1,2,3: 1*16 + 2*4 + 3 = 27.
BEZEL_TEST(owningarray_sort_constraint_gate_3325, control_self_comparable_interface_still_sorts) {
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
        "    using var arr := new OwningArray<IFoo>(); "
        "    arr.Add(new Node(3)); arr.Add(new Node(1)); arr.Add(new Node(2)); "
        "    arr.Sort(); "
        "    return arr.Get(0).Rank() * 16 + arr.Get(1).Rank() * 4 + arr.Get(2).Rank(); "
        "  end Main; end C; "), 27);
}
