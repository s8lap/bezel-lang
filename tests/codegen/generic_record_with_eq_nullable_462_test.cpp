// Codegen — issue #462: THREE codegen sites still assumed the OLD unspecialized
// i32 placeholder layout for a GENERIC RECORD's fields, even after #460 made
// `recordStruct` specialize the struct layout per-instantiation (honoring
// genericSubst_, caching a specialized StructType per distinct field-type vector
// in specializedStructs_). The three sites:
//
//   A (CRASH)     — the `with`-expression copy of a generic record used the i32
//                   placeholder (4 bytes), truncating an 8-byte field. For a
//                   reference field (string / class ptr) this corrupted the
//                   pointer → SIGSEGV; for an 8-byte value field (long/double/
//                   2-int record) it silently lost 4 bytes → wrong value.
//   B (MISCOMPILE)— synthesized record `=` / GetHashCode / ToString over a
//                   generic field used the placeholder layout AND, for a
//                   reference field (string), bit-compared the POINTER instead
//                   of doing value-equality. So `Box<string>(a) = Box<string>(b)`
//                   with a, b distinct pointers to equal content was `false`.
//   C (MISCOMPILE)— the nullable-value wrapper (`Box<int>?`) computed the record
//                   payload's field offsets with the placeholder size, so
//                   `b!.value` read the wrong offset → garbage.
//
// THE FIX routes all three sites through the SAME specialized struct #460 added:
// set up genericSubst_/genericSubstSemaTy_ from the record's concrete type args
// before calling recordStruct, so the layout (size + field offsets) and the
// per-field equality/hash/format dispatch use the substituted concrete types.
//
// Cites §9 (records: with / structural equality / ToString), §7 (nullable),
// §14 (generics are monomorphized). All tests use only real stdlib surface
// (string.Length, §3). Each test crashed or miscompiled before the fix and
// passes after; the always-on IR verifier proves the IR is well-formed.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// BUG A — `with`-expression on a generic record (§9.5 non-destructive update)
// ═══════════════════════════════════════════════════════════════════════════

// §9.5 / §14.1: `with` on Box<string> replacing the string field. Before the
// fix the copy used the i32 placeholder (4 bytes), truncating the 8-byte string
// pointer → SIGSEGV when `.value.Length` dereferenced it. The replacement
// "world" has Length 5.
BEZEL_TEST(generic_record_with_462, with_box_string_replace_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1 := new Box<string>(\"hi\"); "
        "  var b2 := b1 with value := \"world\" end; "
        "  return b2.value.Length; "             // 5
        "end Main; end C;"), 5);
}

// §9.5 / §14.1: the replaced string round-trips (not just its Length) — the
// stored pointer survives the specialized copy + read. Equal to "world" → 1.
BEZEL_TEST(generic_record_with_462, with_box_string_roundtrips) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1 := new Box<string>(\"hi\"); "
        "  var b2 := b1 with value := \"world\" end; "
        "  return (1 if b2.value = \"world\" else 0); "
        "end Main; end C;"), 1);
}

// §9.5 / §14.1: `with` is non-destructive — the SOURCE Box<string> is unchanged.
// b1.value stays "hi" (Length 2) after b2 replaces it.
BEZEL_TEST(generic_record_with_462, with_box_string_source_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1 := new Box<string>(\"hi\"); "
        "  var b2 := b1 with value := \"world\" end; "
        "  return b1.value.Length * 100 + b2.value.Length; "  // 2*100 + 5 = 205
        "end Main; end C;"), 205);
}

// §9.5 / §14.1: `with` on Pair<int, string> replacing the INT field. Both fields
// must be correct afterward — the second (string) field must survive at its
// specialized 8-byte offset, the first replaced. a=7, b.Length=5 → 705.
BEZEL_TEST(generic_record_with_462, with_pair_int_string_replace_int) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair<A, B>(a: A, b: B); "
        "class C public static method Main(): int "
        "  var p1 := new Pair<int, string>(1, \"hello\"); "
        "  var p2 := p1 with a := 7 end; "
        "  Console.WriteLine(p2.a * 100 + p2.b.Length); return 0; "    // 7*100 + 5 = 705
        "end Main; end C;"), "705\n");
}

// §9.5 / §14.1: `with` on Pair<int, string> replacing the STRING field. The int
// field survives, the string replaced. a=3, b="planet".Length=6 → 306.
BEZEL_TEST(generic_record_with_462, with_pair_int_string_replace_string) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair<A, B>(a: A, b: B); "
        "class C public static method Main(): int "
        "  var p1 := new Pair<int, string>(3, \"x\"); "
        "  var p2 := p1 with b := \"planet\" end; "
        "  Console.WriteLine(p2.a * 100 + p2.b.Length); return 0; "    // 3*100 + 6 = 306
        "end Main; end C;"), "306\n");
}

// §9.5 / §14.1: `with` on Box<long> — an 8-byte VALUE field. The placeholder i32
// would silently lose the upper 4 bytes; the replaced value must keep full i64
// width. Replace with a value > 2^32 and compare equal → 1.
BEZEL_TEST(generic_record_with_462, with_box_long_full_width) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1 := new Box<long>(10L); "
        "  var b2 := b1 with value := 5000000042L end; "
        "  return (1 if b2.value = 5000000042L else 0); "
        "end Main; end C;"), 1);
}

// §9.5 / §14.1: `with` on Box<double> — an 8-byte floating value field. Replace
// and read back: 6.5 compared equal → 1.
BEZEL_TEST(generic_record_with_462, with_box_double_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1 := new Box<double>(1.0); "
        "  var b2 := b1 with value := 6.5 end; "
        "  return (1 if b2.value = 6.5 else 0); "
        "end Main; end C;"), 1);
}

// §9.5 / §14.1: `with` on Box<Point> where Point is a 2-int (8-byte) record. The
// nested record value must be copied/replaced at the full specialized size.
// Replace with Point(3,4): 3*10 + 4 = 34.
BEZEL_TEST(generic_record_with_462, with_box_value_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1 := new Box<Point>(new Point(1, 1)); "
        "  var b2 := b1 with value := new Point(3, 4) end; "
        "  return b2.value.X * 10 + b2.value.Y; "  // 34
        "end Main; end C;"), 34);
}

// ── BUG A regressions: value-type T `with` must remain correct ───────────────

// CONTROL (§9.5 / §14.1): `with` on Box<int> — the i32 placeholder coincides
// with int; must still work. Replace 1 → 9 → 9.
BEZEL_TEST(generic_record_with_462, control_with_box_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1 := new Box<int>(1); "
        "  var b2 := b1 with value := 9 end; "
        "  return b2.value; "                    // 9
        "end Main; end C;"), 9);
}

// CONTROL (§9.5 / §14.1): `with` on Box<Named> where Named carries a string.
// The replaced nested record's string field must round-trip. Name "abc".Length=3,
// Age=7 → 307.
BEZEL_TEST(generic_record_with_462, control_with_box_record_with_string) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Named(Name: string, Age: int); "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1 := new Box<Named>(new Named(\"x\", 1)); "
        "  var b2 := b1 with value := new Named(\"abc\", 7) end; "
        "  Console.WriteLine(b2.value.Name.Length * 100 + b2.value.Age); return 0; "  // 3*100 + 7 = 307
        "end Main; end C;"), "307\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// BUG B — synthesized equality / hashing / ToString over a generic field
// (§9.1 structural equality; §8.3 Equals/GetHashCode; §8.5/§9.1 ToString)
// ═══════════════════════════════════════════════════════════════════════════

// §9.1 / §8.3 / §14.1: `Box<string>(a) = Box<string>(b)` where a, b are DISTINCT
// pointers to EQUAL content. The synthesized `=` must compare the string field by
// VALUE (content), not by pointer. Building "hel" + "lo" yields a fresh pointer
// distinct from the literal "hello". Equal → 1.
BEZEL_TEST(generic_record_eq_462, box_string_equal_distinct_pointers) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var a := \"hel\" + \"lo\"; "
        "  var x := new Box<string>(a); "
        "  var y := new Box<string>(\"hello\"); "
        "  return (1 if x = y else 0); "         // content-equal → 1
        "end Main; end C;"), 1);
}

// §9.1 / §8.3 / §14.1: `Box<string>` over DIFFERENT strings must be NOT equal.
// x="hello", y="world" → x = y is false → 0.
BEZEL_TEST(generic_record_eq_462, box_string_not_equal_different) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var x := new Box<string>(\"hello\"); "
        "  var y := new Box<string>(\"world\"); "
        "  return (1 if x = y else 0); "         // different → 0
        "end Main; end C;"), 0);
}

// §9.1 / §8.3 / §14.1: `<>` (not-equal) over content-equal Box<string> → false.
// a built dynamically; x <> y must be false → return 0.
BEZEL_TEST(generic_record_eq_462, box_string_notequal_op_content_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var a := \"wor\" + \"ld\"; "
        "  var x := new Box<string>(a); "
        "  var y := new Box<string>(\"world\"); "
        "  return (1 if x <> y else 0); "        // content-equal → <> is 0
        "end Main; end C;"), 0);
}

// §9.1 / §8.3 / §14.1: Pair<string, int> equality — the string field compares by
// content, the int by value. Distinct-pointer equal strings + equal ints → 1.
BEZEL_TEST(generic_record_eq_462, pair_string_int_structural_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair<A, B>(a: A, b: B); "
        "class C public static method Main(): int "
        "  var s := \"fo\" + \"o\"; "
        "  var x := new Pair<string, int>(s, 42); "
        "  var y := new Pair<string, int>(\"foo\", 42); "
        "  return (1 if x = y else 0); "         // 1
        "end Main; end C;"), 1);
}

// §9.1 / §8.3 / §14.1: Pair<string, int> — equal string but DIFFERENT int → not
// equal → 0. Confirms the int field participates.
BEZEL_TEST(generic_record_eq_462, pair_string_int_diff_int_not_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair<A, B>(a: A, b: B); "
        "class C public static method Main(): int "
        "  var x := new Pair<string, int>(\"foo\", 42); "
        "  var y := new Pair<string, int>(\"foo\", 99); "
        "  return (1 if x = y else 0); "         // 0
        "end Main; end C;"), 0);
}

// §8.3 / §14.1: the hash-equality invariant — equal Box<string> values must hash
// identically (`if a.Equals(b) then a.GetHashCode() = b.GetHashCode()`). Build
// content-equal strings via distinct pointers; the synthesized GetHashCode must
// fold the string field by CONTENT (not pointer bits) so the hashes match → 1.
BEZEL_TEST(generic_record_eq_462, box_string_gethashcode_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var a := \"hel\" + \"lo\"; "
        "  var x := new Box<string>(a); "
        "  var y := new Box<string>(\"hello\"); "
        "  return (1 if x.GetHashCode() = y.GetHashCode() else 0); "  // 1
        "end Main; end C;"), 1);
}

// §9.1 / §8.5 / §14.1: ToString of Box<string> renders the VALUE, not the
// pointer. Format is "Box(value: hello)" — its Length is 17. Reading the length
// of ToString proves the string content was formatted (an i32 placeholder would
// have read garbage / a wrong field). "Box(value: hello)" = 17 chars.
BEZEL_TEST(generic_record_eq_462, box_string_tostring_shows_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"hello\"); "
        "  return b.ToString().Length; "         // "Box(value: hello)" = 17
        "end Main; end C;"), 17);
}

// §9.1 / §8.5 / §14.1: ToString of Box<string> CONTAINS the value substring.
// Confirms (beyond Length) that the actual content "hello" was rendered.
BEZEL_TEST(generic_record_eq_462, box_string_tostring_contains_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"hello\"); "
        "  return (1 if b.ToString().Contains(\"hello\") else 0); "  // 1
        "end Main; end C;"), 1);
}

// §9.1 / §8.3 / §14.1: `.Equals()` method form (not just the `=` operator) over
// content-equal Box<string> with distinct pointers → true → 1.
BEZEL_TEST(generic_record_eq_462, box_string_equals_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var a := \"hel\" + \"lo\"; "
        "  var x := new Box<string>(a); "
        "  var y := new Box<string>(\"hello\"); "
        "  return (1 if x.Equals(y) else 0); "   // 1
        "end Main; end C;"), 1);
}

// ── BUG B regressions: value-type T equality must remain correct ─────────────

// CONTROL (§9.1 / §8.3): Box<int> equality — same int → equal → 1.
BEZEL_TEST(generic_record_eq_462, control_box_int_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var x := new Box<int>(42); "
        "  var y := new Box<int>(42); "
        "  return (1 if x = y else 0); "         // 1
        "end Main; end C;"), 1);
}

// CONTROL (§9.1 / §8.3): Box<int> with different ints → not equal → 0.
BEZEL_TEST(generic_record_eq_462, control_box_int_not_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var x := new Box<int>(42); "
        "  var y := new Box<int>(7); "
        "  return (1 if x = y else 0); "         // 0
        "end Main; end C;"), 0);
}

// CONTROL (§9.1 / §8.3): Box<Named> over a nested record carrying a string — the
// nested record's string field must compare by content (transitivity). Distinct
// pointers, equal content → equal → 1.
BEZEL_TEST(generic_record_eq_462, control_box_record_with_string_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var s := \"al\" + \"ice\"; "
        "  var x := new Box<Named>(new Named(s, 30)); "
        "  var y := new Box<Named>(new Named(\"alice\", 30)); "
        "  return (1 if x = y else 0); "         // 1
        "end Main; end C;"), 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// BUG C — nullable wrapper around a generic record (§7 nullable; §14.1 generics)
// ═══════════════════════════════════════════════════════════════════════════

// §7 / §14.1: `Box<int>?` — the nullable-value wrapper's record payload offsets
// must use the specialized struct. `b!.value` reads the int field → 42.
BEZEL_TEST(generic_record_nullable_462, nullable_box_int_unwrap_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b: Box<int>? := new Box<int>(42); "
        "  return b!.value; "                    // 42
        "end Main; end C;"), 42);
}

// §7 / §14.1: `Box<string>?` — the payload field is a string pointer (8 bytes);
// the placeholder i32 would corrupt it. `b!.value.Length` reads "hello" → 5.
BEZEL_TEST(generic_record_nullable_462, nullable_box_string_unwrap_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b: Box<string>? := new Box<string>(\"hello\"); "
        "  return b!.value.Length; "             // 5
        "end Main; end C;"), 5);
}

// §7.5 / §14.1: `b?.value` (null-conditional) on a non-null Box<string>? — the
// reference field reads at the specialized 8-byte offset and lifts to a nullable
// string; `s!.Length` then reads "hello".Length → 5. (The value-field `?.` +
// `??` lift is exercised by an orthogonal, pre-existing path that is broken for
// NON-generic records too — `Point?` `(b?.X) ?? 0` returns 0 — so it is out of
// this issue's scope; the layout fix #462 lands is proven by `b!.value` and this
// reference-field `?.value`.)
BEZEL_TEST(generic_record_nullable_462, nullable_box_string_null_conditional) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b: Box<string>? := new Box<string>(\"hello\"); "
        "  var s := b?.value; "
        "  return s!.Length; "                   // 5
        "end Main; end C;"), 5);
}

// §7.5 / §14.1: the NULL case — `b?.value` on a null Box<string>? is null (the
// reference field's nullable lift yields null when the receiver is null), so the
// `??` coalesce takes the default. This exercises the reference-field branch's
// null handling at the specialized layout. → 99.
BEZEL_TEST(generic_record_nullable_462, nullable_box_string_null_case) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b: Box<string>? := null; "
        "  var s := b?.value; "
        "  return s!.Length if s is not null else 99; "  // 99
        "end Main; end C;"), 99);
}

// §7 / §14.1: a null check on a nullable generic record. `b is null` after
// assigning a value → false → take the else branch → 1.
BEZEL_TEST(generic_record_nullable_462, nullable_box_string_is_not_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b: Box<string>? := new Box<string>(\"hi\"); "
        "  return (0 if b is null else 1); "     // 1
        "end Main; end C;"), 1);
}

// CONTROL (§7): a NON-generic nullable record `Point?` still works (the existing
// path the issue says is correct). b!.X * 10 + b!.Y → 34.
BEZEL_TEST(generic_record_nullable_462, control_nullable_nongeneric_point) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var b: Point? := new Point(3, 4); "
        "  return b!.X * 10 + b!.Y; "            // 34
        "end Main; end C;"), 34);
}
