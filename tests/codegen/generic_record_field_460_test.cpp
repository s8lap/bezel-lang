// Codegen — issue #460: a single-module GENERIC RECORD instantiated with a
// REFERENCE-type type argument must lay its T-typed field out as the SPECIALIZED
// concrete type per instantiation, the same way a generic CLASS already does
// (§14.1 generics are monomorphized; §9 a record is a value type).
//
// THE BUG (verified): `record Box<T>(value: T)` instantiated as `Box<string>`
// laid the `value` field out as the unspecialized i32 generic placeholder, while
// field access (`b.value.Length`) emitted a `%bz.string` GEP. The base of the
// GEP was therefore an i32 → malformed IR caught by the always-on verifier:
//   "GEP base pointer is not a vector or a vector of pointers"
//   "Load operand must be a pointer."
//
// VERIFIED BOUNDARY (the diagnostic clue):
//   - Box<int>  worked   — the i32 placeholder coincidentally equals int.
//   - Box<long>/<double>/<Pair>/<Named> worked — value-type layouts differ
//     from i32 but the access side read them as such consistently.
//   - Box<string> / Box<SomeClass> were MALFORMED — reference args lower to a
//     pointer field, but the placeholder layout was i32.
//   - class Box<T> with Box<string> already WORKED — generic CLASSES specialize
//     their struct layout per instantiation; generic RECORDS did not.
//
// THE FIX mirrors the class path: recordStruct, when a generic substitution is
// active, builds the field types under that substitution and caches a SEPARATE
// specialized struct per distinct field-type vector (specializedStructs_), so
// Box<int> and Box<string> get distinct LLVM struct types with the correct
// field layout, and field access GEPs against the matching layout.
//
// All tests use only real stdlib surface (string.Length, §3). Each test
// produced malformed IR (or a miscompile) before the fix and passes after; the
// test compiling clean through the always-on IR verifier proves the IR is
// well-formed.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The verified repro: Box<string> field access ─────────────────────────────

// §9 / §14.1: `record Box<T>(value: T)` with T = string. The `value` field is a
// managed string reference (a pointer, §3); `.value.Length` must read the
// string's length. Before the fix the record laid `value` out as i32 →
// malformed GEP/load.  "hello".Length == 5.
BEZEL_TEST(generic_record_string_field_460, box_string_value_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var b := new Box<string>(\"hello\"); "
        "  return b.value.Length; "             // 5
        "end Main; end C;"), 5);
}

// §9 / §14.1: round-trip the string itself (not just its Length) — the stored
// pointer must survive layout + read. Compare against a literal: equal → 1.
BEZEL_TEST(generic_record_string_field_460, box_string_value_roundtrips) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var b := new Box<string>(\"hello\"); "
        "  return (1 if b.value = \"hello\" else 0); "  // §20 if-expr
        "end Main; end C;"), 1);
}

// ── Box<SomeClass>: a class-type type argument ───────────────────────────────

// §9 / §14.1: `record Box<T>(value: T)` with T = a user class. The field is a
// heap pointer; reading the class's int field through the boxed reference must
// give the stored value (7). Before the fix the field was laid out as i32 →
// the class pointer was truncated.
BEZEL_TEST(generic_record_string_field_460, box_class_value_field_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Thing public n: int; "
        "  public constructor(x: int) n := x; end constructor; "
        "  destructor end destructor; end Thing; "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var t := new Thing(7); "
        "  var b := new Box<Thing>(t); "
        "  return b.value.n; "                   // 7
        "end Main; end C;"), 7);
}

// ── Two-field generic record, mixed value + reference args ────────────────────

// §9 / §14.1: `record Pair<A, B>(a: A, b: B)` instantiated as Pair<int, string>.
// The first field is an i32 value, the second a string pointer — the two-field
// layout must specialize each independently. Read both: a=42, b.Length=5 → 4205.
BEZEL_TEST(generic_record_string_field_460, pair_int_string_both_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair<A, B>(a: A, b: B); "
        "class C public static method Main(): int "
        "  using var p := new Pair<int, string>(42, \"hello\"); "
        "  return p.a * 100 + p.b.Length; "      // 42*100 + 5 = 4205
        "end Main; end C;"), 4205);
}

// §9 / §14.1: Pair<string, int> — reference field FIRST, value field second.
// Confirms the per-field specialization is order-independent. b=9, a.Length=5
// → 905.
BEZEL_TEST(generic_record_string_field_460, pair_string_int_both_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair<A, B>(a: A, b: B); "
        "class C public static method Main(): int "
        "  using var p := new Pair<string, int>(\"hello\", 9); "
        "  return p.b * 100 + p.a.Length; "      // 9*100 + 5 = 905
        "end Main; end C;"), 905);
}

// ── Two distinct instantiations of the same generic record coexist ────────────

// §14.1: Box<int> and Box<string> in the SAME program must get DISTINCT
// specialized layouts (the i32-field struct and the ptr-field struct) — the fix
// caches them separately. Box<int>.value + Box<string>.value.Length = 30 + 5.
BEZEL_TEST(generic_record_string_field_460, box_int_and_box_string_coexist) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var bi := new Box<int>(30); "
        "  using var bs := new Box<string>(\"hello\"); "
        "  return bi.value + bs.value.Length; "  // 30 + 5 = 35
        "end Main; end C;"), 35);
}

// ── Regressions — value-type T must remain correct ───────────────────────────

// CONTROL (§9 / §14.1): Box<int> — the i32 placeholder coincides with int's
// layout; this already passed and must stay correct.
BEZEL_TEST(generic_record_string_field_460, control_box_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(42); "
        "  return b.value; "                     // 42
        "end Main; end C;"), 42);
}

// CONTROL (§9 / §14.1): Box<long> — a WIDER value type; the field must be i64,
// not truncated to i32. A value above 2^32 would be clobbered by an i32 layout;
// compare the field to the same long literal — equal (→ 1) proves full width.
// (The field is compared rather than `as int`-cast to dodge an UNRELATED,
// pre-existing front-end limitation on `T`-typed `as int` casts that reproduces
// on generic CLASSES too, not just records.)
BEZEL_TEST(generic_record_string_field_460, control_box_long) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var b := new Box<long>(5000000000L); "
        "  return (1 if b.value = 5000000000L else 0); "  // full i64 width
        "end Main; end C;"), 1);
}

// CONTROL (§9 / §14.1): Box<Pair> where Pair is a value-type RECORD. A nested
// record value field — both inner fields survive (already worked; must remain).
BEZEL_TEST(generic_record_string_field_460, control_box_value_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var b := new Box<Pair>(new Pair(10, 20)); "
        "  return b.value.A * 1000 + b.value.B; "  // 10020
        "end Main; end C;"), 10020);
}

// CONTROL (§9 / §14.1): Box<Named> where Named is a record CONTAINING a string —
// the nested record's own string field must round-trip (the record value carries
// a pointer + an int; both survive). Name.Length=5, Age=6 → 5006.
BEZEL_TEST(generic_record_string_field_460, control_box_record_with_string_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var b := new Box<Named>(new Named(\"hello\", 6)); "
        "  return b.value.Name.Length * 1000 + b.value.Age; "  // 5006
        "end Main; end C;"), 5006);
}

// ── Generic record returned from a method ────────────────────────────────────

// §9 / §14.1: a Box<string> returned from a (non-generic) method — the
// specialized record value crosses the call boundary intact; .value.Length == 5.
BEZEL_TEST(generic_record_string_field_460, box_string_returned_from_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C "
        "  public static method Make(): Box<string> => new Box<string>(\"hello\"); "
        "  public static method Main(): int "
        "    var b := Make(); "
        "    return b.value.Length; "            // 5
        "  end Main; end C;"), 5);
}

// ── Nested generic-record substitution chaining ──────────────────────────────

// §9 / §14.1: a generic record `Box<U>` as a FIELD of a generic class
// `Holder<U>`. With U = string the OUTER class subst (U → ptr) must flow into
// the INNER record's field layout when `_b: Box<U>` is lowered as a field type
// and when `_b.value` is read. Reads "hello".Length == 5.
BEZEL_TEST(generic_record_string_field_460, generic_record_field_in_generic_class) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class Holder<U> "
        "  private _b: Box<U>; "
        "  public constructor(v: U) _b := new Box<U>(v); end constructor; "
        "  public method Get(): U => _b.value; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<string>(\"hello\"); "
        "  return h.Get().Length; "             // 5
        "end Main; end C;"), 5);
}

// §9 / §14.1: a non-generic record value (`Point`) as the type argument of a
// generic record, accessed while no record-relevant subst would otherwise be
// active for `Point`. Box<Point> reads both inner fields: 3*10 + 4 = 34.
BEZEL_TEST(generic_record_string_field_460, box_nongeneric_record_in_generic_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var b := new Box<Point>(new Point(3, 4)); "
        "  var inner := b.value; "
        "  return inner.X * 10 + inner.Y; "     // 34
        "end Main; end C;"), 34);
}
