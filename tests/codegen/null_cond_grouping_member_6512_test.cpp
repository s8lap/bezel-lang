// #6512 (stdlib §5 / spec §7.5, v0.338): `?.` on an `IGrouping<TKey, T>`
// receiver typed nothing and lowered to nothing, so the enclosing store was
// dropped, the program kept its previous value, and it exited 0 answering
// WRONGLY — signalled only by the `internal:` #2961 backstop.
//
// THE RULE. §7.5: "`x?.M` returns null if `x` is null; otherwise it performs the
// access. The result type is always nullable." There is NO nullability
// precondition on the receiver, so `?.` on the non-nullable grouping a `for`
// loop over `IGrouping<TKey, T>` yields performs the access and only the RESULT
// is lifted. (§7.5's sibling rejection, #5452, is about a TYPE NAME — a static
// receiver that is not a value and is never null. A non-nullable VALUE receiver
// is admitted, as it already was for string, Array and user classes.)
//
// WHAT §5 DECLARES, AND WHY THE RESULT IS A VALUE. stdlib §5 declares exactly
// one own member on IGrouping — `readonly property Key: TKey` — and §5's
// `public extension IEnumerable<T>` declares `public method Count(): int`. Both
// spellings therefore name REAL members, so the correct result is a value, not
// a diagnostic: `g?.Key` is `TKey?` and `g?.Count()` is `int?`.
//
// WHY NOTHING LOWERED. IGrouping TypeRefs are synthetic (no declaration), so in
// resolveNullCondMember `memberTargetType` returns null, `lookupMember` finds
// nothing, and the access fell through both the generic member path and the
// #3155 member-not-found catch-all (whose `knownCompleteUserType` gate requires
// a non-null `targetType`). A `?.` call's callee is a NullCondMemberExpr, so
// typeCall's IGrouping LINQ receiver arm — keyed on a MemberAccessExpr callee —
// never ran either. In codegen a `?.` call reaches neither the generic method
// dispatch (`ms` is null: a synthetic TypeRef carries no MemberSymbol) nor
// tryLowerBuiltinClassCall's IGrouping `Count()` arm, and a `?.` member is a
// distinct AST node so the MemberAccessExpr `bz_grouping_key` arm never ran
// either. Each of those gaps alone drops the value; MEASURED on the pre-fix
// tree, `n + g?.Count()` returned 0 where the `n + g.Count()` control returned 4.
//
// THE TESTS. Every value test is runAot/runAotChecked and asserts the RUNTIME
// VALUE — `exitCode == 0` alone is not an oracle here, since the pre-fix
// behavior exits 0 with the wrong number. Each value test carries its `.`-form
// control in the same program or as a sibling, so a fix that merely produced
// SOME value (e.g. lowered to 0, or took the `??` default) fails.
// `runAotChecked` additionally gives the ASan/LSan leg.
//
// KNOWN-SIBLING GAP, deliberately NOT pinned here: other §5 LINQ operators on a
// grouping receiver (`g?.First()`, `g?.ToArray()`, …) are still untyped and
// degrade to #2961. This issue's rule covers the two members §5 declares on the
// `IGrouping` receiver itself plus its inherited `Count()` operator; widening to
// the whole LINQ surface is a separate change, filed separately.

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"           // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The issue's exact repro shape: `n := n + g?.Count()` over `GroupBy`. The
// `?? -1` default is a SENTINEL — if the `?.` access were dropped or
// short-circuited, every group would contribute -1 and n would be -2, not 4.
// [1,2,3,4] by `x mod 2` → key 0: {2,4}, key 1: {1,3}: two groups of two, so the
// sum of the counts is 4 — exactly what the `g.Count()` control returns.
const char* kNullCondCountSum =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3, 4]; "
    "    var n := 0; "
    "    for var g in a.GroupBy(lambda(x: int) => x mod 2) do "
    "      n := n + (g?.Count() ?? -1); "
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

// Both spellings of BOTH members in one program, so the `?.` forms are pinned to
// the `.` forms rather than merely to "some value". Per group:
// g.Count()=2, g?.Count()=2, and g.Key / g?.Key are the group key (0 then 1):
//   key 0: 2 + 2 + 0 + 0 = 4
//   key 1: 2 + 2 + 1 + 1 = 6      → 10
// A `?.` form that lowered to 0 or took the default (-1) yields a different sum.
const char* kNullCondAgreesWithDotForm =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3, 4]; "
    "    var n := 0; "
    "    for var g in a.GroupBy(lambda(x: int) => x mod 2) do "
    "      n := n + g.Count() + (g?.Count() ?? -1) + g.Key + (g?.Key ?? -1); "
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

// The `.`-form control on its own — the value the `?.` form must reproduce
// (`g.Key`: keys 0 and 1, sum 1).
const char* kDotFormKeySum =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3, 4]; "
    "    var n := 0; "
    "    for var g in a.GroupBy(lambda(x: int) => x mod 2) do "
    "      n := n + g.Key; "
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

// `g?.Key` through a `??` — the one member stdlib §5 declares as IGrouping's
// OWN property (`readonly property Key: TKey`), so it must be `TKey?`. Same sum
// as the `.`-form control above: keys 0 and 1 → 1.
const char* kNullCondKeySum =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3, 4]; "
    "    var n := 0; "
    "    for var g in a.GroupBy(lambda(x: int) => x mod 2) do "
    "      n := n + (g?.Key ?? -1); "
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

// The `?.` access is PERFORMED, never short-circuited: `g` is non-nullable, so
// the §7.4 narrowing check must hold for every group. The value is FOLDED IN,
// not merely counted — a bare `seen + 1` behind the narrowing check passed on
// the pre-fix tree (the degraded slot compared non-equal to null and the branch
// was taken with a zero value), so it asserted nothing. Folding `c` in makes the
// test fail on pre-fix code: there the narrowing check is false for every group
// (MEASURED 0), with the fix the two counts 2 + 2 sum to 4.
const char* kNullCondCountIsPresent =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3, 4]; "
    "    var seen := 0; "
    "    for var g in a.GroupBy(lambda(x: int) => x mod 2) do "
    "      var c := g?.Count(); "
    "      if c <> null then seen := seen + (c ?? 0); end; "
    "    end; "
    "    return seen; "
    "  end Main; "
    "end C;";

// Same for the key — folded value, keys 0 and 1 sum to 1 (MEASURED 0 pre-fix).
const char* kNullCondKeyIsPresent =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3, 4]; "
    "    var seen := 0; "
    "    for var g in a.GroupBy(lambda(x: int) => x mod 2) do "
    "      var k := g?.Key; "
    "      if k <> null then seen := seen + (k ?? -1); end; "
    "    end; "
    "    return seen; "
    "  end Main; "
    "end C;";

// The TYPE is `int?` / `TKey?`, not a bare `int` / `TKey`: §7.11 forbids the
// arithmetic operators over `T?`, so `int + g?.Count()` must be REJECTED. These
// two pin the diagnostic that SURVIVES the fix (the sibling `internal:` #2961
// backstop is the one that dies, so it must not be the needle).
const char* kCountNotLiftedOverAddition =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3, 4]; "
    "    var n := 0; "
    "    for var g in a.GroupBy(lambda(x: int) => x mod 2) do "
    "      n := n + g?.Count(); "
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

const char* kKeyNotLiftedOverAddition =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3, 4]; "
    "    var n := 0; "
    "    for var g in a.GroupBy(lambda(x: int) => x mod 2) do "
    "      n := n + g?.Key; "
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

const char* kNullableOperandNeedle =
    "operator `+` is not defined on a nullable value type";

}  // namespace

// =====================================================================
// #6512 (stdlib §5 / §7.5, v0.338) — the `?.` spelling of an IGrouping
// member is a VALUE with the correct type and the correct runtime result.

BEZEL_TEST(null_cond_grouping_member_6512,
           nullcond_count_sums_group_sizes_spec_5_7_5) {
    JitResult r = runAotChecked(kNullCondCountSum);
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // two groups of two; NOT 0 (pre-fix), NOT -2
}

BEZEL_TEST(null_cond_grouping_member_6512,
           nullcond_count_and_key_agree_with_dot_form_spec_5_7_5) {
    JitResult r = runAotChecked(kNullCondAgreesWithDotForm);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);  // (2+2+0+0) + (2+2+1+1)
}

BEZEL_TEST(null_cond_grouping_member_6512,
           dot_form_key_sum_is_the_control_spec_5) {
    JitResult r = runAotChecked(kDotFormKeySum);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // keys 0 and 1
}

BEZEL_TEST(null_cond_grouping_member_6512,
           nullcond_key_reads_the_group_key_spec_5_7_5) {
    JitResult r = runAotChecked(kNullCondKeySum);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // same as the `.`-form control above
}

BEZEL_TEST(null_cond_grouping_member_6512,
           nullcond_count_is_performed_not_short_circuited_spec_7_5) {
    JitResult r = runAotChecked(kNullCondCountIsPresent);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // two groups of two; MEASURED 0 pre-fix
}

BEZEL_TEST(null_cond_grouping_member_6512,
           nullcond_key_is_performed_not_short_circuited_spec_7_5) {
    JitResult r = runAotChecked(kNullCondKeyIsPresent);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // keys 0 and 1; MEASURED 0 pre-fix
}

BEZEL_TEST(null_cond_grouping_member_6512,
           nullcond_count_is_nullable_int_spec_7_5_7_11) {
    JitResult r = runAot(kCountNotLiftedOverAddition);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNullableOperandNeedle));
}

BEZEL_TEST(null_cond_grouping_member_6512,
           nullcond_key_is_nullable_key_spec_7_5_7_11) {
    JitResult r = runAot(kKeyNotLiftedOverAddition);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNullableOperandNeedle));
}