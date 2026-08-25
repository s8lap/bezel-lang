// Spec-derived regression tests for #2783 (v0.240): `Array<T>.MinOrNull()` /
// `Array<T>.MaxOrNull()` — the `*OrNull` siblings of Min()/Max() in stdlib
// §5.1's `IEnumerable<T> where T: IComparable<T>` extension.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §5.1 declares all four in ONE block:
//
//     public extension IEnumerable<T> where T: IComparable<T>
//       public method Min(): T;              // throws if empty
//       public method Max(): T;              // throws if empty
//       public method MinOrNull(): T?;       // null if empty
//       public method MaxOrNull(): T?;       // null if empty
//     end;
//
// §5.1's empty-source contract distinguishes them explicitly: "`Min`/`Max`
// — selector and no-selector forms alike — and `Average` throw
// `InvalidOperationException`", while "the `*OrNull` variants return `null`
// instead".  Min()/Max() resolve on an `Array<T>` receiver today; the two
// `*OrNull` siblings from the *same* declaration did NOT.
//
// THE DEFECT — a SEMANTIC reject in the call-form validator only.  The
// `ExprTC::typeCall` no-member reject at
// `ExprTypecheck_Calls.cpp::allowedNonOwningMethod` /
// `allowedOwningMethod` was a positive-list of names a non-owning / owning
// CONTAINER collection call is permitted to use; it listed `Min` and `Max`
// but NOT `MinOrNull`/`MaxOrNull`, so a `arr.MinOrNull()` call on any
// Array<T> (numeric, string, class, or interface element) was rejected with
// "no member `MinOrNull` on `Array<T>` (§22.5 / §4.x) — it is not part of
// this collection's method surface, a universal Object member, or a LINQ
// operator" — the exact message #2484's stdlib §5 surface transcription was
// supposed to suppress.  Measured on `5cebfbc`.  The bare-reference form was
// ALREADY handled correctly by #2484/#2492 (`isLinqOperator` admits these
// names, but the call-form dispatch through `allowedNonOwningMethod` /
// `allowedOwningMethod` did not).
//
// THE FIX adds `MinOrNull`/`MaxOrNull` to BOTH the non-owning allowlist and
// the owning allowlist (the owning family has them too: §28.4's owning
// collection also extends `IEnumerable<T>`, so `usingOwningArray.MinOrNull()`
// must compile for the same reason), and adds a no-selector codegen dispatch
// in `tryLowerCollectionInstanceCall` that mirrors Min/Max but returns T?
// (null on empty source) instead of throwing.  Selector overloads
// (`MinOrNull<U>(sel: (T)->U)`) and decimal elements are out of scope here —
// file a follow-up if needed.
//
// THE TEST PINS both the integer-element success path (Array<int>) and the
// interface-element success path (Array<IQux>), both the non-empty branch
// (returns the min/max value packed into a discriminator), and the empty-
// source branch (returns null and the test program returns 9).  The packed
// value form `Min*10 + Max` mirrors #2770's so this test sits next to it
// without artifact collision.
//
// All four cases are runAotChecked (generated-code ASan) — the interface case
// walks the same `bz$ifacecmp$<Iface>` thunk #2770 introduced, so a wrong
// thunk pointer / slot-address mistake surfaces here rather than as a value
// coincidence.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #2783 / stdlib §5.1: `Array<int>.MinOrNull()` / `.MaxOrNull()`.
//
// Two-element array {5, 2}: MinOrNull = 2, MaxOrNull = 5.  Packed as
// Min*10 + Max = 25, well under runAotChecked's 8-bit WEXITSTATUS mask, so
// no truncation is involved and no decomposition is needed.
//
// Without the fix, the call-form validator at
// `allowedNonOwningMethod` rejects the program with the "no member
// `MinOrNull`" message — the test exits -999 (no run).
BEZEL_TEST(array_minornull_maxornull_2783, min_max_ornull_int_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    var arr: Array<int> := new Array<int>(3); "
        "    arr.Add(5); "
        "    arr.Add(2); "
        "    var mn: int? := arr.MinOrNull(); "
        "    var mx: int? := arr.MaxOrNull(); "
        "    if mn = null then return 1; end; "
        "    if mx = null then return 2; end; "
        "    return mn! * 10 + mx!; "
        "  end Main; "
        "end C; "), 25);
}

// #2783 / stdlib §5.1: empty `Array<int>.MinOrNull()` returns null.  The
// `*OrNull` family differs from `Min`/`Max` EXACTLY on this branch — the
// former returns null, the latter throws.  Packed discriminator 9 (the
// null-returns-9 convention from the issue's repro).
//
// Without the fix, the call-form validator rejects the program with the
// "no member" message — the test exits -999.
BEZEL_TEST(array_minornull_maxornull_2783, min_ornull_int_element_empty_returns_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    var arr: Array<int> := new Array<int>(3); "
        "    var mn: int? := arr.MinOrNull(); "
        "    if mn = null then return 9; end; "
        "    return 7; "
        "  end Main; "
        "end C; "), 9);
}

// #2783 / stdlib §5.1: `Array<IQux>.MinOrNull()` / `.MaxOrNull()` where IQux
// is an INTERFACE element extending `IComparable<IQux>` (§14.2 rule 3).
//
// Elements {Rank=3, Rank=1}: Min = 1, Max = 3.  Packed Min*10 + Max = 13.
// The interface case is the strongest — codegen walks the same
// `bz$ifacecmp$<Iface>` thunk #2770 introduced, so a wrong thunk pointer /
// slot-address mistake surfaces as an ASan read of unrelated memory here
// rather than as a value coincidence.  Packed as 13 (matching #2770's
// discriminator) so this test sits cleanly next to it.
//
// Without the fix, the call-form validator rejects the program with the
// "no member" message — the test exits -999.
BEZEL_TEST(array_minornull_maxornull_2783, min_max_ornull_iface_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Linq; "
        "public interface IQux: IComparable<IQux> "
        "  method Rank(): int; "
        "end IQux; "
        "class QNode: IQux "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IQux): int => _r - other.Rank(); "
        "end QNode; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new QNode(3); "
        "    using var b := new QNode(1); "
        "    var arr: Array<IQux> := [a, b]; "
        "    var mn: IQux? := arr.MinOrNull(); "
        "    var mx: IQux? := arr.MaxOrNull(); "
        "    if mn = null then return 1; end; "
        "    if mx = null then return 2; end; "
        "    return mn!.Rank() * 10 + mx!.Rank(); "
        "  end Main; "
        "end C; "), 13);
}

// #2783 / stdlib §5.1: empty `Array<IQux>.MinOrNull()` returns null — the
// sibling of the int-element empty test, on the interface element.  Uses
// the null-returns-9 / non-null-returns-7 discriminator from the issue's
// repro.  (The packing differs from the int cases because Rank has wider
// values than 0..9 and would risk WEXITSTATUS truncation.)
//
// Without the fix, the call-form validator rejects the program with the
// "no member" message — the test exits -999.
BEZEL_TEST(array_minornull_maxornull_2783, min_ornull_iface_element_empty_returns_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Linq; "
        "public interface IQux: IComparable<IQux> "
        "  method Rank(): int; "
        "end IQux; "
        "class QNode: IQux "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IQux): int => _r - other.Rank(); "
        "end QNode; "
        "class C "
        "  public static method Main(): int "
        "    var arr: Array<IQux> := new Array<IQux>(); "
        "    var mn: IQux? := arr.MinOrNull(); "
        "    if mn = null then return 9; end; "
        "    return 7; "
        "  end Main; "
        "end C; "), 9);
}