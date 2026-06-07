// Codegen — structural equality of record string fields compares by CONTENT
// (§8.3, §9.1, §9.2), not by pointer identity.
//
// §8.3: "the record's value is determined by its fields"; the auto-generated
// `=` / `<>` / `Equals` / `GetHashCode` are memberwise over those fields, and
// strings carry value semantics (§4.6) — two strings are equal iff their code
// points are equal, independent of which heap object holds them. A record with
// equal string content therefore compares equal even when its string fields are
// distinct allocations. This must agree with the synthesized GetHashCode, which
// already hashes strings by content (§8.3 hash-equality invariant).
//
// To make the by-content (not by-pointer) distinction observable, the two
// records under test are built from independently-materialized strings: each
// string literal lowers to its own global, and one operand is additionally
// produced by concatenation (`+`), a fresh heap allocation. A by-pointer
// comparison would report these unequal; by-content reports them equal.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §8.3 / §9.1: positional record with a single string field — equal content
// (one side built by concatenation so the pointers differ) compares EQUAL.
BEZEL_TEST(codegen_record_str_eq, positional_string_equal_content_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(value: string); "
        "class C public static method Main(): int "
        "  var a := new Name(\"Ann\"); "
        "  var b := new Name(\"An\" + \"n\"); "  // distinct heap string
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// §8.3: `<>` is the negation of `=` — equal string content → `<>` is false.
BEZEL_TEST(codegen_record_str_eq, positional_string_neq_false_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(value: string); "
        "class C public static method Main(): int "
        "  var a := new Name(\"Ann\"); "
        "  var b := new Name(\"An\" + \"n\"); "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 0);
}

// §9.1: the synthesized `.Equals()` is the same structural comparison as `=`
// (shared emitStructEquality) — equal string content → true.
BEZEL_TEST(codegen_record_str_eq, positional_string_equals_method_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(value: string); "
        "class C public static method Main(): int "
        "  var a := new Name(\"Ann\"); "
        "  var b := new Name(\"An\" + \"n\"); "
        "  return (1 if a.Equals(b) else 0); "
        "end Main; end C;"), 1);
}

// §8.3: different string content compares UNEQUAL (`=` false, `<>` true).
BEZEL_TEST(codegen_record_str_eq, positional_string_different_content_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(value: string); "
        "class C public static method Main(): int "
        "  var a := new Name(\"Ann\"); "
        "  var b := new Name(\"Bob\"); "
        "  return (1 if (a = b) or (a <> b = false) else 0); "
        "end Main; end C;"), 0);
}

// §8.3 / §9.1: a mixed string+int positional record — both fields must match.
// Equal string content AND equal int → equal.
BEZEL_TEST(codegen_record_str_eq, mixed_string_int_all_equal_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Person(name: string, age: int); "
        "class C public static method Main(): int "
        "  var a := new Person(\"Ann\", 30); "
        "  var b := new Person(\"An\" + \"n\", 30); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// §8.3: mixed record — equal string content but differing int → UNEQUAL.
BEZEL_TEST(codegen_record_str_eq, mixed_string_int_int_differs_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Person(name: string, age: int); "
        "class C public static method Main(): int "
        "  var a := new Person(\"Ann\", 30); "
        "  var b := new Person(\"An\" + \"n\", 31); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}

// §8.3: mixed record — equal int but differing string content → UNEQUAL.
BEZEL_TEST(codegen_record_str_eq, mixed_string_int_string_differs_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Person(name: string, age: int); "
        "class C public static method Main(): int "
        "  var a := new Person(\"Ann\", 30); "
        "  var b := new Person(\"Bob\", 30); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}

// §8.3 hash-equality invariant: equal records hash equal. With string fields now
// compared by content, `=` and GetHashCode (which already hashes strings by
// content, §4.6) agree — equal content → equal hash AND equal records.
BEZEL_TEST(codegen_record_str_eq, gethashcode_agrees_with_equality_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Person(name: string, age: int); "
        "class C public static method Main(): int "
        "  var a := new Person(\"Ann\", 30); "
        "  var b := new Person(\"An\" + \"n\", 30); "
        "  return (1 if a = b and a.GetHashCode() = b.GetHashCode() else 0); "
        "end Main; end C;"), 1);
}

// §9.2: a body record with a string auto-property — equal content compares EQUAL
// (the synthesis iterates auto-properties through the same emitStructEquality).
BEZEL_TEST(codegen_record_str_eq, body_record_string_auto_property_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Greeting "
        "  public property Text: string; "
        "  public constructor(text: string) Text := text; end constructor; "
        "end Greeting; "
        "class C public static method Main(): int "
        "  var a := new Greeting(\"hi\"); "
        "  var b := new Greeting(\"h\" + \"i\"); "
        "  return (1 if a.Equals(b) "
        "             and a.GetHashCode() = b.GetHashCode() else 0); "
        "end Main; end C;"), 1);
}

// §9.2: a body record with a string auto-property — differing content is
// UNEQUAL via the synthesized Equals.
BEZEL_TEST(codegen_record_str_eq, body_record_string_differs_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Greeting "
        "  public property Text: string; "
        "  public constructor(text: string) Text := text; end constructor; "
        "end Greeting; "
        "class C public static method Main(): int "
        "  var a := new Greeting(\"hi\"); "
        "  var b := new Greeting(\"bye\"); "
        "  return (1 if a.Equals(b) else 0); "
        "end Main; end C;"), 0);
}

// §9.1 (transitivity): a nested record whose inner record has a string field —
// the recursion into emitStructEquality must also content-compare the inner
// string. Equal nested string content → equal outer records.
BEZEL_TEST(codegen_record_str_eq, nested_record_inner_string_equal_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Label(text: string); "
        "record Tagged(label: Label, id: int); "
        "class C public static method Main(): int "
        "  var a := new Tagged(new Label(\"ok\"), 1); "
        "  var b := new Tagged(new Label(\"o\" + \"k\"), 1); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// §9.1: nested record — differing inner string content is detected → UNEQUAL.
BEZEL_TEST(codegen_record_str_eq, nested_record_inner_string_differs_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Label(text: string); "
        "record Tagged(label: Label, id: int); "
        "class C public static method Main(): int "
        "  var a := new Tagged(new Label(\"ok\"), 1); "
        "  var b := new Tagged(new Label(\"no\"), 1); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// §8.3 / §9.1 — NULLABLE string fields (`string?`).
//
// A `string?` lowers to a plain `ptr` whose null is a real null pointer (it has
// no hasValue tag, unlike a nullable value primitive). The synthesized
// value-semantics members (`=`, `<>`, `Equals`, `GetHashCode`, `ToString`)
// must null-guard such a field before any content operation, or a null field
// SIGSEGVs (loadStringLen on a null pointer). The contract:
//   equality:    both-null → equal; exactly-one-null → not-equal;
//                neither-null → content equality.
//   GetHashCode: a null field hashes to the fixed sentinel 0.
//   ToString:    a null field renders as the literal `null` (the same
//                rendering null interpolation uses).
// Each of these crashes without the null guard; they are regression tests for
// that class of crash.
// ---------------------------------------------------------------------------

// §8.3: both `string?` fields null → records compare EQUAL via `=`. (The
// same-pointer fast path already collapses two real nulls, but the guard must
// keep this true once it short-circuits before loadStringLen.)
BEZEL_TEST(codegen_record_str_eq, nullable_string_both_null_equal_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(value: string?); "
        "class C public static method Main(): int "
        "  var a := new Name(null); "
        "  var b := new Name(null); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// §8.3: exactly one `string?` field null → NOT equal (and would SIGSEGV in the
// content compare without the one-null guard). `=` is false.
BEZEL_TEST(codegen_record_str_eq, nullable_string_one_null_not_equal_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(value: string?); "
        "class C public static method Main(): int "
        "  var a := new Name(null); "
        "  var b := new Name(\"x\"); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}

// §8.3: `<>` is the negation of `=` — exactly one field null → `<>` is true.
BEZEL_TEST(codegen_record_str_eq, nullable_string_one_null_neq_true_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(value: string?); "
        "class C public static method Main(): int "
        "  var a := new Name(null); "
        "  var b := new Name(\"x\"); "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 1);
}

// §9.1: the synthesized `.Equals()` is the same comparison as `=` — both fields
// null → true; one null → false.
BEZEL_TEST(codegen_record_str_eq, nullable_string_equals_method_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Data(msg: string?); "
        "class C public static method Main(): int "
        "  var a := new Data(null); "
        "  var b := new Data(null); "
        "  var c := new Data(\"x\"); "
        "  return (1 if a.Equals(b) and not a.Equals(c) else 0); "
        "end Main; end C;"), 1);
}

// §8.3: a null `string?` field hashes (its sentinel) consistently — two records
// with a null field hash EQUAL; a record with a non-null field hashes
// DIFFERENTLY. Without the guard, GetHashCode null-derefs the field.
BEZEL_TEST(codegen_record_str_eq, nullable_string_gethashcode_null_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Data(msg: string?); "
        "class C public static method Main(): int "
        "  var a := new Data(null); "
        "  var b := new Data(null); "
        "  var c := new Data(\"x\"); "
        "  return (1 if a.GetHashCode() = b.GetHashCode() "
        "             and a.GetHashCode() <> c.GetHashCode() else 0); "
        "end Main; end C;"), 1);
}

// §8.5 / §9.1: ToString of a record with a null `string?` field renders the
// field as `null` (matching null interpolation); a non-null field renders its
// content. Without the guard, ToString null-derefs the field.
BEZEL_TEST(codegen_record_str_eq, nullable_string_tostring_null_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Msg(opt: string?); "
        "class C public static method Main(): int "
        "  var m := new Msg(null); "
        "  var n := new Msg(\"hi\"); "
        "  return (1 if m.ToString() = \"Msg(opt: null)\" "
        "             and n.ToString() = \"Msg(opt: hi)\" else 0); "
        "end Main; end C;"), 1);
}

// §9.1: a record with a null nullable field round-trips through all of value
// semantics in one program (equality both-null, Equals, hash, ToString) — the
// combined regression for the SIGSEGV class.
BEZEL_TEST(codegen_record_str_eq, nullable_string_field_roundtrip_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(s: string?, n: int); "
        "class C public static method Main(): int "
        "  var a := new Box(null, 7); "
        "  var b := new Box(null, 7); "
        "  return (1 if a = b "
        "             and a.Equals(b) "
        "             and a.GetHashCode() = b.GetHashCode() "
        "             and a.ToString() = \"Box(s: null, n: 7)\" else 0); "
        "end Main; end C;"), 1);
}

// §8.3 / §9.1: the guard is generic over NULLABLE REFERENCE fields, not just
// string. A nullable class field (`Inner?`) set to null must likewise compare
// equal (both-null), hash to the sentinel, and render `null` — never call
// ToString on a null `this` nor deref. (Equality/hash were already non-crashing
// for class refs; ToString previously rendered the type name and risked a
// null-`this` ToString call — the guard makes all three consistent.)
BEZEL_TEST(codegen_record_str_eq, nullable_class_field_null_value_semantics_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Inner public V: int := 0; "
        "  public override method ToString(): string return \"inner!\"; end ToString; "
        "end Inner; "
        "record Holder(inner: Inner?); "
        "class C public static method Main(): int "
        "  var a := new Holder(null); "
        "  var b := new Holder(null); "
        "  return (1 if a = b "
        "             and a.GetHashCode() = b.GetHashCode() "
        "             and a.ToString() = \"Holder(inner: null)\" else 0); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// §8.3 / §8.5 / §9.1 — NULLABLE VALUE fields (`int?`, `bool?`, …).
//
// A nullable value primitive lowers to a TAGGED STRUCT {i8 hasValue; T value}
// (isNullablePrimTy / nullablePrimStructTy / wrapNullablePrim), unlike a
// nullable reference (a plain `ptr`). The synthesized value-semantics members
// must read the hasValue tag rather than treating the struct as raw bytes:
//   equality (§8.4): both-null → equal; exactly-one-null → not-equal;
//                    neither-null → the inner value's `=`. (`null = null` is
//                    true, `null = nonNull` is false — §8.4.)
//   GetHashCode:     hasValue → hash the inner value; null → the fixed sentinel
//                    0 (so two both-null fields hash identically, §8.3 invariant).
//   ToString:        hasValue → the inner value's ToString; null → `null`.
// Before the fix, the synthesis compared/hashed/rendered the hasValue byte
// (so two equal non-null values compared UNEQUAL, ToString printed the tag `1`,
// and a null field rendered `0`). Each test fails without the tag-aware fix.
// ---------------------------------------------------------------------------

// §8.3 / §8.4: two `int?` fields with equal non-null values compare EQUAL.
// (Pre-fix: false — the hasValue byte matched but the value slot was never
// reached, yet the raw-bytes compare actually compared {tag,value} as a struct
// and reported equal only when both were null; a non-null pair compared unequal
// because the value-vs-tag mixing differed.) `=` is true.
BEZEL_TEST(codegen_record_str_eq, nullable_int_equal_value_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(n: int?); "
        "class C public static method Main(): int "
        "  return (1 if new Box(5) = new Box(5) else 0); "
        "end Main; end C;"), 1);
}

// §8.3 / §8.4: two `int?` fields with different non-null values compare UNEQUAL.
// `=` is false, `<>` is true.
BEZEL_TEST(codegen_record_str_eq, nullable_int_different_value_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(n: int?); "
        "class C public static method Main(): int "
        "  return (1 if new Box(5) <> new Box(6) and (new Box(5) = new Box(6)) = false "
        "             else 0); "
        "end Main; end C;"), 1);
}

// §8.4: both `int?` fields null → EQUAL (`null = null` is true).
BEZEL_TEST(codegen_record_str_eq, nullable_int_both_null_equal_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(n: int?); "
        "class C public static method Main(): int "
        "  return (1 if new Box(null) = new Box(null) else 0); "
        "end Main; end C;"), 1);
}

// §8.4: exactly one `int?` field null → NOT equal (`nonNull = null` is false).
BEZEL_TEST(codegen_record_str_eq, nullable_int_one_null_not_equal_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(n: int?); "
        "class C public static method Main(): int "
        "  return (1 if (new Box(5) = new Box(null)) = false "
        "             and (new Box(null) = new Box(5)) = false else 0); "
        "end Main; end C;"), 1);
}

// §9.1: the synthesized `.Equals()` is the same structural comparison as `=` —
// equal non-null `int?` → true, both-null → true.
BEZEL_TEST(codegen_record_str_eq, nullable_int_equals_method_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(n: int?); "
        "class C public static method Main(): int "
        "  return (1 if new Box(7).Equals(new Box(7)) "
        "             and new Box(null).Equals(new Box(null)) "
        "             and new Box(7).Equals(new Box(null)) = false else 0); "
        "end Main; end C;"), 1);
}

// §8.3: GetHashCode satisfies the hash-equality invariant — equal records
// (equal non-null value, or both-null) hash identically; the null field uses
// the fixed sentinel that the both-null pair collapses to.
BEZEL_TEST(codegen_record_str_eq, nullable_int_gethashcode_agrees_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(n: int?); "
        "class C public static method Main(): int "
        "  return (1 if new Box(42).GetHashCode() = new Box(42).GetHashCode() "
        "             and new Box(null).GetHashCode() = new Box(null).GetHashCode() "
        "             else 0); "
        "end Main; end C;"), 1);
}

// §8.5: a non-null `int?` field renders its VALUE (not its hasValue tag `1`).
// Pre-fix it printed `Box(n: 1)`.
BEZEL_TEST(codegen_record_str_eq, nullable_int_tostring_value_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(n: int?); "
        "class C public static method Main(): int "
        "  return (1 if new Box(5).ToString() = \"Box(n: 5)\" "
        "             and new Box(99).ToString() = \"Box(n: 99)\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: a NULL `int?` field renders the literal `null` (not `0`). Pre-fix it
// printed `Box(n: 0)`.
BEZEL_TEST(codegen_record_str_eq, nullable_int_tostring_null_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(n: int?); "
        "class C public static method Main(): int "
        "  return (1 if new Box(null).ToString() = \"Box(n: null)\" else 0); "
        "end Main; end C;"), 1);
}

// §9.2: the body-record (auto-property) form of an `int?` field — its auto
// ToString renders the property VALUE, not the hasValue tag. Pre-fix it printed
// `Box(N: 1)`.
BEZEL_TEST(codegen_record_str_eq, nullable_int_body_record_tostring_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box "
        "  public property N: int?; "
        "  public constructor(n: int?) N := n; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  return (1 if new Box(5).ToString() = \"Box(N: 5)\" "
        "             and new Box(null).ToString() = \"Box(N: null)\" else 0); "
        "end Main; end C;"), 1);
}

// §8.3 / §8.5: a second nullable value type (`bool?`) — equality reads the tag
// (true=true equal, true<>false, both-null equal), and ToString renders the
// boolean value when present and `null` when absent.
BEZEL_TEST(codegen_record_str_eq, nullable_bool_value_semantics_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Flag(b: bool?); "
        "class C public static method Main(): int "
        "  return (1 if new Flag(true) = new Flag(true) "
        "             and (new Flag(true) = new Flag(false)) = false "
        "             and new Flag(null) = new Flag(null) "
        "             and new Flag(true).GetHashCode() = new Flag(true).GetHashCode() "
        "             and new Flag(true).ToString() = \"Flag(b: True)\" "  // #452: bool.ToString() → "True"/"False" per stdlib
        "             and new Flag(null).ToString() = \"Flag(b: null)\" else 0); "
        "end Main; end C;"), 1);
}

// §8.3 / §9.1: a nested record inherits the tag-aware nullable-value handling
// through the structural recursion (emitStructEquality / emitRecordHashCode /
// emitRecordToString drive the inner record's fields by their declared types).
// Equal non-null inner `int?` → equal; the rendering shows the inner value.
BEZEL_TEST(codegen_record_str_eq, nested_record_nullable_int_inherits_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Inner(n: int?); record Outer(inner: Inner); "
        "class C public static method Main(): int "
        "  var a := new Outer(new Inner(8)); "
        "  var b := new Outer(new Inner(8)); "
        "  var c := new Outer(new Inner(null)); "
        "  var d := new Outer(new Inner(null)); "
        "  return (1 if a = b and c = d and (a = c) = false "
        "             and a.GetHashCode() = b.GetHashCode() "
        "             and a.ToString() = \"Outer(inner: Inner(n: 8))\" "
        "             and c.ToString() = \"Outer(inner: Inner(n: null))\" else 0); "
        "end Main; end C;"), 1);
}
