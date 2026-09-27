// Regression tests for issue #5078 — §5.1 "Nullable conversions", v0.307:
// "`S?` implicitly converts to `T?` whenever `S` implicitly converts to `T` —
// a present value converts by the underlying conversion, `null` converts to
// `null`", at an ARRAY-ELEMENT / DICTIONARY-VALUE INDEXER assignment
// (`arr[i] := ki;` / `arr[i] ??= ki;` / `d[k] := ki;` / `d[k] ??= ki;`) where
// the target's element/value type is a nullable value type narrower or wider
// than the RHS's own nullable-value type.
//
// The §17 review of fix #5017 found that #5017's `liftNullableValueToSlot`
// lift (LlvmBackend_Stmts.cpp) is reached for a local, an instance field, a
// static field, and an auto-property target — but NOT for an array-element or
// dictionary-value indexer target. Indexer assignment is a SEPARATE
// `IndexExpr` arm in `lowerStmt` that `return`s before the shared preamble
// where #5017's fix lives, and `lowerCollectionIndexerSet`'s (LlvmBackend_
// Assign.cpp) own Array<T?> and Dictionary<K,T?> nullable-lift blocks only
// handled a BARE scalar or a null-literal `val` — not `val` already being the
// tagged `{i8, S}` struct of a DIFFERENT nullable-value type. MEASURED on
// origin/main e3b0bdf0 and independently on r22-5017 (3bed2ab9/2c561136
// applied — confirming this is pre-existing, not a #5017 regression):
//
//   var arr: Array<double?> := [null, null];
//   var ki: int? := 4;
//   arr[0] := ki;
//
// crashed the compile:
//
//   Invalid InsertValueInst operands!
//     %18 = insertvalue { i8, double } { i8 1, double undef }, { i8, i32 } %12, 1
//   bezelc: codegen error: IR verification failed (malformed IR emitted; see report above)
//
// and identically for `??=`, a null source, an unsigned-source zero-extend,
// and a `Dictionary<string, long?>` value target.
//
// The fix hoists #5017's `liftNullableValueToSlot` lambda to a member
// function (LlvmBackend_impl.hpp) and calls it from
// `lowerCollectionIndexerSet`'s Array<T?> element block and its Dictionary
// `encodeValue` lambda, trying the unwrap/convert/rewrap lift first and
// falling back to the pre-existing bare-scalar / inner-record handling only
// when it declines (i.e. `val` is not itself the tagged shape).
//
// `OwningArray<T?>` / `IOwningList<T?>` are NOT covered here: stdlib §4.2 /
// language spec §14.4 constrain the owning collection family to `where T:
// class`, and a nullable VALUE type does not satisfy it — MEASURED,
// `OwningArray<int?>` is rejected at compile time ("`OwningArray` requires a
// class or interface element type … a primitive value type has no disposal
// obligation and cannot be owned in an owning collection"), so the crash
// shape this issue describes cannot be constructed for the owning family.
//
// The `Dictionary` `??=` row below deliberately does NOT build its
// precondition from a `["k": null]` DICTIONARY LITERAL (unlike the `:=` row
// above it, which never reads the old value and so never trips this).
// MEASURED, independently of #5078 and out of this fix's scope: a
// `Dictionary<K, V?>` literal entry whose VALUE is `null` is encoded as a
// bare null pointer rather than a boxed `{i8=0, T}` struct — confirmed via
// `BEZEL_DUMP_IR=1` (`bz_dict_set_str(...,  ptr null)` at the literal site,
// vs. every indexer-SET path's `nullableValueNull` + malloc/store box).  Any
// later READ of that entry (an indexer get, `d[k] = null`, or `??=`'s own
// getter) then dereferences that null pointer and SIGSEGVs — reproducible
// with no `??=`/`:=` involved at all:
//   var d: Dictionary<string, long?> := ["k": null];
//   if d["k"] = null then ...   // segfault reading the entry
// A second, likely related pre-existing crash: overwriting an EXISTING
// non-null dictionary value with a `null` literal (`d["k"] := null;` where
// "k" already holds a real boxed value) aborts with `free(): invalid
// pointer` — the disposing-overwrite runtime call is handed something it
// does not expect for the null replacement. Filed as issue #5084; not fixed
// here. This row instead builds its precondition with a FRESH dictionary and
// an INSERT (not overwrite) of `null` via `:=`, which does not touch either
// defect (MEASURED clean), so it exercises only the #5078 lift.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 (v0.307), fix #5078 — the issue's wrong-value repro verbatim
// (site A, `:=`). Pre-fix: IR verification failure ("Invalid InsertValueInst
// operands!").
BEZEL_TEST(nullable_lift_index_assign_5078, array_index_assign_int_nullable_to_double_nullable) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var arr: Array<double?> := [null, null]; "
        "    var ki: int? := 4; "
        "    arr[0] := ki; "
        "    var v: double := arr[0]!; "
        "    Console.WriteLine($\"arr {v}\"); "
        "  end Main; "
        "end C;",
        "arr 4\n");
}

// per §5.1 / §7.7 (v0.307), fix #5078 — the `??=` spelling into the same
// array-element slot. Pre-fix: same IR verification failure.
BEZEL_TEST(nullable_lift_index_assign_5078, array_index_coalesce_assign_int_nullable_to_double_nullable) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var arr: Array<double?> := [null]; "
        "    var ki: int? := 4; "
        "    arr[0] ??= ki; "
        "    var v: double := arr[0]!; "
        "    Console.WriteLine($\"coaleq_arr {v}\"); "
        "  end Main; "
        "end C;",
        "coaleq_arr 4\n");
}

// per §5.1 (v0.307, "`null` converts to `null`"), fix #5078 — a null source
// must leave the slot null (not crash, and not the parked-4fe44cd6-class
// regression of rewrapping with hasValue = 1 unconditionally). The target
// starts PRESENT (7.0) so a store that fails to update the tag would read
// back as present rather than by accident already being null.
BEZEL_TEST(nullable_lift_index_assign_5078, array_index_assign_null_source_stays_null) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var arr: Array<double?> := [7.0]; "
        "    var ki: int? := null; "
        "    arr[0] := ki; "
        "    if arr[0] = null then "
        "      Console.WriteLine(\"arr null\"); "
        "    else "
        "      Console.WriteLine(\"arr not-null\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "arr null\n");
}

// per §5.1 (v0.307), fix #5078 — an unsigned source converts by its own
// signedness (`uint` 4000000000 → `long`, not a negative sign-extension).
// The target starts at -1 so a partial (payload-low-bytes-only) store cannot
// read back the right value by accident.
BEZEL_TEST(nullable_lift_index_assign_5078, array_index_assign_unsigned_source_zero_extends) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var arr: Array<long?> := [-1]; "
        "    var u: uint? := 4000000000; "
        "    arr[0] := u; "
        "    var v: long := arr[0]!; "
        "    Console.WriteLine($\"u {v}\"); "
        "  end Main; "
        "end C;",
        "u 4000000000\n");
}

// per §5.1 (v0.307), fix #5078 — the DICTIONARY-VALUE twin (`d[k] := ki;`):
// `lowerCollectionIndexerSet`'s `encodeValue` lambda conflated an already-
// tagged struct of a DIFFERENT nullable-value type with a bare inner record
// and handed it straight to `wrapNullableValue`. Pre-fix: same IR
// verification failure as the Array<T?> shape, on
// `Dictionary<string, long?>["k"] := ki;`.
BEZEL_TEST(nullable_lift_index_assign_5078, dictionary_index_assign_int_nullable_to_long_nullable) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<string, long?> := [\"k\": null]; "
        "    var ki: int? := 4; "
        "    d[\"k\"] := ki; "
        "    var v: long := d[\"k\"]!; "
        "    Console.WriteLine($\"dict {v}\"); "
        "  end Main; "
        "end C;",
        "dict 4\n");
}

// per §5.1 / §7.7 (v0.307), fix #5078 — the `??=` spelling into the same
// dictionary-value slot. Pre-fix: same IR verification failure. The
// precondition inserts `null` into a FRESH dictionary via `:=` rather than a
// `["k": null]` literal — see the file header comment: the literal spelling
// trips an unrelated, pre-existing crash (issue #5084) in dictionary-literal
// null-value encoding, MEASURED independent of this fix.
BEZEL_TEST(nullable_lift_index_assign_5078, dictionary_index_coalesce_assign_int_nullable_to_long_nullable) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<string, long?> := new Dictionary<string, long?>(); "
        "    d[\"k\"] := null; "
        "    var ki: int? := 4; "
        "    d[\"k\"] ??= ki; "
        "    var v: long := d[\"k\"]!; "
        "    Console.WriteLine($\"coaleq_dict {v}\"); "
        "  end Main; "
        "end C;",
        "coaleq_dict 4\n");
}
