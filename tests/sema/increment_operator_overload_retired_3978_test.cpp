// Spec-derived regression test for issue #3978 (§25.1 "Overloadable
// operators" / §17.6 "The increment and decrement operators", v0.295).
//
// THE DEFECT. The compiler implemented a RETIRED v0.236 reading of §17.6:
// "For a value-type operand whose type T overloads `operator ++`, ... a
// reference-type operand [that] overloads the operator" — admitting BOTH
// (a) a `method operator ++`/`operator --` DECLARATION, and (b) `x++`/`x--`
// on a class/record-typed operand whose type declares one.
//
// §25.1 (v0.295), verbatim, in the *not overloadable* list:
//
//   "The increment and decrement statements `++` and `--` — built-in
//    statements over the operand types §6.6's compound form admits
//    (§17.6), not operators of the expression grammar; a type spells
//    advancement as `x := x + 1` under its own `operator +`, or as a named
//    method (the shape the standard library's `ISteppable<T>.Step` already
//    takes)."
//
// §17.6 (v0.295), verbatim, the operative sentence:
//
//   "On an operand of any other type the statement is a compile error on a
//    separate ground: `++`/`--` are not user-overloadable (§25.1), so a
//    type spells advancement as `x := x + 1` under its own `operator +`,
//    or as a named method."
//
// So there are two independent rules, and pre-fix the compiler enforced
// neither:
//   1. a `method operator ++`/`operator --` declaration is not admissible
//      at all (checked here via TypeResolution_Checks.cpp's
//      `checkOperatorGroups`, which already rejects `not`/`!`/`??`/... and
//      the compound-assignment tokens by the same mechanism);
//   2. `x++`/`x--` on a class or record operand is a compile error, whether
//      or not the type declares such an overload (checked here via
//      ExprTypecheck.cpp's `IncrementStmt` handler, which used to look for
//      a matching `operator ++`/`operator --` and admit the statement when
//      one was found — the escape hatch this fix removes).
//
// ⚠ The issue's OWN posted repro used `using var a := new Ct(3); a++;`,
// which the issue's own review comment found trips a DIFFERENT,
// pre-existing rejection first (fix #830: "using var is implicitly
// non-reassignable") before ever reaching the §25.1/§17.6 gap. The
// corrected repro (confirmed in the issue thread) swaps `using var` for a
// plain `var` with an explicit `.Dispose()` — that is the shape used below
// (T5/T6).
//
// MEASURED on the pre-fix compiler (R17 integration head 2504380): every
// REJECT row below compiled with rc=0 and zero diagnostics.
//
// FALSE-REJECT COST: every ACCEPT control below (T7-T14) is unaffected by
// this fix — none of them resolves a Named-type operand through the
// removed overload-lookup branch (they are built-in numeric types, an
// array/indexer element, a narrowed nullable primitive, and a bare generic
// type parameter, none of which enter the `isRefType || isValueTypeRecord`
// branch this fix changes). See the lane's accept-side sweep log for the
// full grid (int/float/decimal/char locals, a field, an inline-array
// element, a heap-array indexer, a for-loop counter, a narrowed `int?`, and
// a generic `T` constrained to `INumber<T>`/`ISteppable<T>` — the last two
// REJECT identically before and after, since a bare type parameter never
// resolved a declaration to search for an overload on either side of this
// fix).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool diagContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

int run(const JitResult& r) { return r.ran ? r.exitCode : -999; }

}  // namespace

// ===========================================================================
// RULE 1 (declaration) — §25.1: `operator ++`/`operator --` is not
// admissible on any declaring type.
// ===========================================================================

// T1: `operator ++` on a RECORD (the §17.6/§25.3 `Counter` shape the retired
// reading admitted).
BEZEL_TEST(increment_operator_overload_retired_3978,
           record_operator_plusplus_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator ++(c: Counter): Counter "
        "    => new Counter(c.value + 1); "
        "end Counter; "
        "class C public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "operator `++` may not be overloaded (§25.1)"));
}

// T2: `operator --` on a CLASS.
BEZEL_TEST(increment_operator_overload_retired_3978,
           class_operator_minusminus_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "class Ct "
        "  public n: int := 0; "
        "  public static method operator --(a: Ct): owned Ct "
        "    => new Ct(); "
        "end Ct; "
        "class C public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "operator `--` may not be overloaded (§25.1)"));
}

// T3: `operator ++` on an ENUM (tests/codegen/enum_operators_1582_test.cpp's
// `postfix_increment_spec_17_6_25_2` pinned this admitted, pre-fix).
BEZEL_TEST(increment_operator_overload_retired_3978,
           enum_operator_plusplus_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator ++(p: Priority): Priority "
        "    => Priority(int(p) + 1); "
        "end Priority; "
        "class C public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "operator `++` may not be overloaded (§25.1)"));
}

// T4: `operator ++` on a CHOICE — §25 admits operator overloads on choices
// too ("§35.2 admits of a choice body exactly what an enum body admits,
// operator overloads included"), so the same declaration-site check must
// fire there as well.
BEZEL_TEST(increment_operator_overload_retired_3978,
           choice_operator_plusplus_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "choice Shape "
        "  Circle(r: int), "
        "  Square(s: int); "
        "  public static method operator ++(a: Shape): Shape => a; "
        "end Shape; "
        "class C public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "operator `++` may not be overloaded (§25.1)"));
}

// ===========================================================================
// RULE 2 (use) — §17.6: `x++`/`x--` on a class or record operand is a
// compile error REGARDLESS of whether the type declares `operator ++`/
// `operator --` (dropping the "unless the type overloads it" escape).
// ===========================================================================

// T5: the issue's CORRECTED repro (plain `var`, not `using var` — see the
// header note) — a class-typed `a++` through a user `operator ++`.
BEZEL_TEST(increment_operator_overload_retired_3978,
           class_plusplus_with_overload_rejected_spec_17_6_issue_3978) {
    auto r = runAot(
        "module M; "
        "class Ct "
        "  public n: int; "
        "  public constructor(v: int) this.n := v; end constructor; "
        "  public static method operator ++(a: Ct): owned Ct "
        "    return new Ct(a.n + 1); "
        "  end operator ++; "
        "  destructor end destructor; "
        "end Ct; "
        "class C "
        "  public static method Main(): int "
        "    var a := new Ct(3); "
        "    a++; "
        "    a.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // Two independent diagnostics are expected on this program: the
    // declaration-site reject (rule 1, T1-T4's check) AND the use-site
    // reject (rule 2) — the fix does not rely on the ordering of the two
    // (see the fix's own comment in ExprTypecheck.cpp).
    BEZEL_EXPECT_TRUE(diagContains(r, "operator `++` may not be overloaded (§25.1)"));
    BEZEL_EXPECT_TRUE(diagContains(r,
        "`++` is not defined on reference type `Ct`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "not user-overloadable (§25.1)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
}

// T6: a RECORD-typed `c--` through a user `operator --` — the §17.6/§2599
// value-type-record sibling.
BEZEL_TEST(increment_operator_overload_retired_3978,
           record_minusminus_with_overload_rejected_spec_17_6) {
    auto r = runAot(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator --(c: Counter): Counter "
        "    => new Counter(c.value - 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(10); "
        "  c--; "
        "  return c.value; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r,
        "`--` is not defined on record type `Counter`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "not user-overloadable (§25.1)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "value-type record"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
}

// ===========================================================================
// ACCEPT CONTROLS — a false reject here is worse than the retired feature.
// None of these resolves a Named-type operand through the removed
// overload-lookup branch.
// ===========================================================================

// T7: `int` local — §6.6/§17.6 category (2), untouched by this fix.
BEZEL_TEST(increment_operator_overload_retired_3978,
           int_local_increment_still_accepted_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var i := 5; i++; return i; "
        "end Main; end C;")), 6);
}

// T8: `double` local.
BEZEL_TEST(increment_operator_overload_retired_3978,
           double_local_increment_still_accepted_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var d: double := 1.0; d++; return int(d); "
        "end Main; end C;")), 2);
}

// T9: `decimal` local — the struct-typed primitive that ICE'd pre-#2858 on
// the property-virtual-dispatch path; here it is the plain local form.
BEZEL_TEST(increment_operator_overload_retired_3978,
           decimal_local_increment_still_accepted_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var m: decimal := 1m; m++; return int(m); "
        "end Main; end C;")), 2);
}

// T10: `char` local.
BEZEL_TEST(increment_operator_overload_retired_3978,
           char_local_increment_still_accepted_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var ch: char := 'a'; ch++; return int(ch); "
        "end Main; end C;")), 98);
}

// T11: an inline-array int element and a heap-array (`Array<int>`) indexer
// element — §17.6's indexer desugaring, unaffected.
BEZEL_TEST(increment_operator_overload_retired_3978,
           array_and_indexer_element_increment_still_accepted_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var arr: [3]int; arr[0]++; "
        "  using var a := new Array<int>(3, 0); a[0]++; "
        "  return arr[0] + a[0]; "
        "end Main; end C;")), 2);
}

// T12: a narrowed `int?` — §7.4's narrowing licenses `n++` exactly as it
// licenses `n := n + 1`.
BEZEL_TEST(increment_operator_overload_retired_3978,
           narrowed_nullable_int_increment_still_accepted_spec_7_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  if n <> null then n++; end if; "
        "  return n ?? -1; "
        "end Main; end C;")), 6);
}

// T13/T14: a bare generic type parameter constrained to `INumber<T>` /
// `ISteppable<T>` — §17.6 governs concrete built-in numeric types only, not
// a type-parameter operand (whose static type never resolved a declaration
// this fix's `isRefType`/`isValueTypeRecord` branches — or the pre-fix
// overload-lookup they replaced — could search for a member on). REJECTS
// identically before and after this fix; not a regression.
BEZEL_TEST(increment_operator_overload_retired_3978,
           generic_inumber_constrained_increment_rejected_both_sides) {
    auto r = runAot(
        "module M; class C "
        "  public static method Bump<T>(x: T): T where T: INumber<T> "
        "    var y := x; y++; return y; "
        "  end Bump; "
        "  public static method Main(): int return int(Bump<int>(5)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

BEZEL_TEST(increment_operator_overload_retired_3978,
           generic_isteppable_constrained_increment_rejected_both_sides) {
    auto r = runAot(
        "module M; class C "
        "  public static method Bump<T>(x: T): T where T: ISteppable<T> "
        "    var y := x; y++; return y; "
        "  end Bump; "
        "  public static method Main(): int return int(Bump<int>(5)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
