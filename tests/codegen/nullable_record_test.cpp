// Codegen — a NULLABLE RECORD value type (`record?`) has a real tagged-optional
// representation (§7.2, §7.4, §7.5, §8.4, §8.6, §17.4; issue #416).
//
// A non-nullable record is a by-value LLVM struct (§9.4) with NO null
// representation. Before this fix `record?` ALSO lowered to the bare record
// struct, so the `null` literal was indistinguishable from a real record value
// and the compiler had no `hasValue` tag to test. `record? <> null`, `!.field`,
// and `?.field` then routed `null` through emitStructEquality (ExtractValue on a
// non-aggregate) and aborted the compiler (LLVM checkGEPType), or silently read
// 0/garbage. A nullable record now lowers, like the other nullable value types
// (int?, bool?, …, enum?), to a tagged struct {i8 hasValue; <record payload>},
// so `null` is representable and distinct from every record value.
//
// These tests exercise: the null literal (§7.2 / §8.4), the null-safe `=`/`<>`
// of §8.4, narrowed member access via `<> null` (§7.4) and `!` (§7.8),
// null-conditional `?.` (§7.5), a nullable-record field, and the §8.6/§17.4 `as`
// boxing round-trip (`o as Rect` yields `Rect?` — finding [11]'s end-to-end
// pattern). REGRESSION GUARDS confirm a NON-null `record` is unchanged: bare
// struct field access, structural equality, and construction still work.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A module with a single-field Box record and a Main body. Reused widely.
int runBox(const std::string& mainBody) {
    return run(
        "module M; record Box(V: int); "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

// A module with a two-field Pair record and a Main body (multi-field payload).
int runPair(const std::string& mainBody) {
    return run(
        "module M; record Pair(A: int, B: int); "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

}  // namespace

// ---- §7.2 / §8.4: the null literal and null-safe `=`/`<>` ------------------

// §7.2: a `null`-initialized `record?` compares equal to `null` (§8.4
// null = null is true). Was: a crash / indistinguishable from a value.
BEZEL_TEST(codegen_nullable_record, null_literal_equals_null_spec_7_2_8_4) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := null; return int(b = null);"), 1);
}

// §8.4: a `null`-initialized `record?` is `<> null` → false.
BEZEL_TEST(codegen_nullable_record, null_literal_ne_null_false_spec_8_4) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := null; return int(b <> null);"), 0);
}

// §8.4: a value-initialized `record?` is NOT null — `= null` is false. This is
// the comparison whose `null` operand previously aborted emitStructEquality
// (finding [11]).
BEZEL_TEST(codegen_nullable_record, value_not_equal_null_spec_8_4) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := new Box(7); return int(b = null);"), 0);
}

// §8.4: `<> null` is true for a value-initialized `record?` (the load-bearing
// condition of finding [11] — was an LLVM checkGEPType abort).
BEZEL_TEST(codegen_nullable_record, value_ne_null_true_spec_8_4) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := new Box(7); return int(b <> null);"), 1);
}

// ---- §7.4: narrowed member access after `if b <> null` ---------------------

// §7.4 / finding [11], the canonical repro: a member read inside the `<> null`
// guard returns the field value (7), not a crash. Was: SIGABRT (checkGEPType).
BEZEL_TEST(codegen_nullable_record, narrowed_member_access_spec_7_4) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := new Box(7); "
        "if b <> null then return b.V; end; return 0;"), 7);
}

// §7.4 / §8.4: the null branch is taken for a null `record?` — the `<> null`
// test correctly distinguishes null from a value (the else arm runs).
BEZEL_TEST(codegen_nullable_record, narrowed_member_null_takes_else_spec_7_4) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := null; "
        "if b <> null then return b.V; else return -1; end;"), -1);
}

// §7.4: a multi-field record narrowed member access reads each field correctly
// (payload offset is applied to every field, not just field 0).
BEZEL_TEST(codegen_nullable_record, narrowed_member_multi_field_spec_7_4) {
    BEZEL_EXPECT_EQ(runPair(
        "var p: Pair? := new Pair(11, 22); "
        "if p <> null then return p.A * 100 + p.B; end; return 0;"), 1122);
}

// ---- §7.8: `!` force-unwrap then member access ----------------------------

// §7.8: `b!` on a non-null `record?` yields the non-null record; `.V` reads the
// field (7). Was: a crash on the `b <> null` condition (finding [11]).
BEZEL_TEST(codegen_nullable_record, bang_unwrap_member_spec_7_8) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := new Box(7); "
        "if b <> null then return b!.V; end; return 0;"), 7);
}

// §7.8: `var nb: Box := b!;` copies the unwrapped record into a non-null local;
// reading its field works (the payload is extracted whole).
BEZEL_TEST(codegen_nullable_record, bang_unwrap_copy_then_member_spec_7_8) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := new Box(7); var nb: Box := b!; return nb.V;"), 7);
}

// ---- §7.5: null-conditional `?.` on a nullable record --------------------

// §7.5 / finding [14]: `b?.V` on a non-null `record?` performs the access and
// yields a non-null `int?` holding the field value (7). Was: returned 0.
BEZEL_TEST(codegen_nullable_record, null_cond_member_present_spec_7_5) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := new Box(7); var x: int? := b?.V; "
        "if x = null then return 200; end; return x! + 50;"), 57);
}

// §7.5: `b?.V` on a null `record?` short-circuits to null (the result type is
// always nullable; the access is not performed).
BEZEL_TEST(codegen_nullable_record, null_cond_member_null_spec_7_5) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box? := null; var x: int? := b?.V; "
        "return int(x = null);"), 1);
}

// §7.5: a multi-field record `?.` reads the requested field (not field 0) when
// present — exercises the payload-relative GEP for a non-zero field index.
BEZEL_TEST(codegen_nullable_record, null_cond_member_multi_field_spec_7_5) {
    BEZEL_EXPECT_EQ(runPair(
        "var p: Pair? := new Pair(11, 22); var x: int? := p?.B; "
        "if x = null then return 200; end; return x!;"), 22);
}

// ---- §7.2: a nullable-record FIELD on a record ---------------------------

// §7.2 / §8.3: a record may have a nullable-record field; reading it back after
// a `<> null` guard yields the inner field value.
BEZEL_TEST(codegen_nullable_record, nullable_record_field_present_spec_7_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(V: int); record Holder(B: Box?); "
        "class C public static method Main(): int "
        "  var h := new Holder(new Box(9)); var b := h.B; "
        "  if b <> null then return b!.V; end; return 0; "
        "end Main; end C;"), 9);
}

// §7.2: a null nullable-record field is null when read back.
BEZEL_TEST(codegen_nullable_record, nullable_record_field_null_spec_7_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(V: int); record Holder(B: Box?); "
        "class C public static method Main(): int "
        "  var h := new Holder(null); var b := h.B; "
        "  return int(b = null); "
        "end Main; end C;"), 1);
}

// ---- §8.6 / §17.4: the `as`-then-null-check boxing round-trip -------------
// The end-to-end pattern from issue #416: a record boxed into Object, unboxed
// via `as` (which yields `Rect?` per §17.4), then narrowed via `<> null` and
// read via `!`. Before this fix the `r <> null` comparison aborted the compiler.

BEZEL_TEST(codegen_nullable_record, as_box_unbox_nullcheck_member_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rect(W: int, H: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Rect(3, 4); var r := o as Rect; "
        "  if r <> null then return r!.W; end; return -1; "
        "end Main; end C;"), 3);
}

// §17.4 / §8.6: `o as Rect` of a boxed Rect is `<> null` (a compatible unbox
// succeeds), and the unboxed payload round-trips both fields.
BEZEL_TEST(codegen_nullable_record, as_box_unbox_both_fields_spec_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rect(W: int, H: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Rect(3, 4); var r := o as Rect; "
        "  if r <> null then return r!.W * 10 + r!.H; end; return -1; "
        "end Main; end C;"), 34);
}

// §17.4: `o as Rect` of a boxed DIFFERENT record yields null (incompatible
// unbox) — the result is a null `Rect?`, so `= null` is true.
BEZEL_TEST(codegen_nullable_record, as_box_unbox_wrong_type_null_spec_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rect(W: int, H: int); record Box(V: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Rect(3, 4); var r := o as Box; "
        "  return int(r = null); "
        "end Main; end C;"), 1);
}

// ---- REGRESSION GUARDS: a NON-null `record` is unchanged ------------------

// §9.1 / §9.4: a plain (non-nullable) record's positional field access still
// reads the bare struct directly (no payload offset).
BEZEL_TEST(codegen_nullable_record, regress_nonnull_record_field_access) {
    BEZEL_EXPECT_EQ(runBox(
        "var b: Box := new Box(7); return b.V;"), 7);
}

// §9.4 / §8.3: a plain non-nullable record still compares structurally — two
// equal records are `=`, and this must NOT go through the nullable-tag path.
BEZEL_TEST(codegen_nullable_record, regress_nonnull_record_equality_equal) {
    BEZEL_EXPECT_EQ(runPair(
        "var a := new Pair(11, 22); var b := new Pair(11, 22); "
        "return int(a = b);"), 1);
}

// §8.3: two non-nullable records with different fields are NOT equal.
BEZEL_TEST(codegen_nullable_record, regress_nonnull_record_equality_neq) {
    BEZEL_EXPECT_EQ(runPair(
        "var a := new Pair(11, 22); var b := new Pair(11, 33); "
        "return int(a = b);"), 0);
}

// §9.4: a plain non-nullable record passed to a method by value, fields read in
// the callee — the bare-struct passing convention is unchanged.
BEZEL_TEST(codegen_nullable_record, regress_nonnull_record_passed_by_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C "
        "  public static method Sum(p: Pair): int => p.A + p.B; "
        "  public static method Main(): int "
        "    return Sum(new Pair(11, 22)); "
        "end Main; end C;"), 33);
}

// §9.4: a multi-field non-nullable record field read still works directly.
BEZEL_TEST(codegen_nullable_record, regress_nonnull_record_multi_field) {
    BEZEL_EXPECT_EQ(runPair(
        "var p := new Pair(11, 22); return p.A * 100 + p.B;"), 1122);
}
