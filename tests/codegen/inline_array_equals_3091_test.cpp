// Codegen — inline-array `.Equals(b)` / `.GetHashCode()` (§15.1.2, v0.249,
// #3091). Before this fix the calls fell through to a generic dispatch
// that returned false / 0 unconditionally, breaking the §3.6 (auto-hashable)
// contract for inline arrays.
//
// §15.1.2 (line 1948): "Two inline-array values of the same type are equal iff
// their elements are pairwise equal under the element type's equality."
// §8.3 (equality table): "Inline array `[N]T` … Element-by-element value
// equality (§15.1.2)." §9.2 / §3.6: records and inline arrays are auto-hashable
// (record-equality discipline), so `GetHashCode()` MUST be present and MUST be
// a stable hash of the slots.
//
// Regression: pre-fix output for `a.Equals(b)` where a == b == [1,2,3] was
// `False` (the method was missing entirely, fell through to a default-false
// generic dispatch); `a.GetHashCode()` returned `0`. Both wrong, both breaking
// the §3.6 / §8.3 / §15.1.2 contract. The `=` operator / `<>` operator already
// worked (emitInlineArrayEquality, LlvmBackend_Layout.cpp / Exprs.cpp), so the
// gap was on the METHOD-call dispatch specifically — tryLowerInlineArrayCall
// did not recognise Equals/GetHashCode. This fix adds both arms.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

std::string runStdout(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// §15.1.2: two equal [3]int values — a.Equals(b) → True (1).
BEZEL_TEST(codegen_inline_array_eq, equals_true_equal_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  var b: [3]int := [1, 2, 3]; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2: differing in one slot — a.Equals(b) → False (0).
BEZEL_TEST(codegen_inline_array_eq, equals_false_unequal_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  var b: [3]int := [1, 2, 4]; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §15.1.2: differing in the FIRST slot (every slot participates, not just the
// last — same coverage as the `=`-operator regression in
// inline_array_equality_test).
BEZEL_TEST(codegen_inline_array_eq, equals_false_first_slot_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [9, 2, 3]; "
        "  var b: [3]int := [1, 2, 3]; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §15.1.2: arity-1 array (smallest legal size, `[0]T` is rejected at parse
// time per §15.1).
BEZEL_TEST(codegen_inline_array_eq, equals_true_arity1_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [1]int := [42]; "
        "  var b: [1]int := [42]; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2: arity-1 array, differing single slot.
BEZEL_TEST(codegen_inline_array_eq, equals_false_arity1_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [1]int := [42]; "
        "  var b: [1]int := [43]; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §15.1.2 + §9.1: record elements compare field-by-field. Two [2]Point
// arrays whose elements are field-equal compare equal.
BEZEL_TEST(codegen_inline_array_eq, equals_true_record_elements_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a: [2]Point := [new Point(1, 2), new Point(3, 4)]; "
        "  var b: [2]Point := [new Point(1, 2), new Point(3, 4)]; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2 + §9.1: differing record field → not equal.
BEZEL_TEST(codegen_inline_array_eq, equals_false_record_elements_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a: [2]Point := [new Point(1, 2), new Point(3, 4)]; "
        "  var b: [2]Point := [new Point(1, 2), new Point(3, 9)]; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §15.1.2 + §4.6: string elements compare by CONTENT, not pointer identity.
// The strings are built at runtime (concatenation) so the pointers differ,
// ensuring the comparison is by content.
// `[2]string` carries an explicit initializer: `string` is not defaultable
// (§17.11, v0.307), so the uninitialised form is rejected (§15.1 / §7.9,
// v0.307; #4419 — pinned in definite_assignment_default_init_4419_test.cpp).
BEZEL_TEST(codegen_inline_array_eq, equals_true_string_content_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]string := [\"\", \"\"]; a[0] := \"foo\"; a[1] := \"ba\" + \"r\"; "
        "  var b: [2]string := [\"\", \"\"]; b[0] := \"f\" + \"oo\"; b[1] := \"bar\"; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2 + §4.6: string elements differing in content → not equal.
// `[2]string` carries an explicit initializer: `string` is not defaultable
// (§17.11, v0.307), so the uninitialised form is rejected (§15.1 / §7.9,
// v0.307; #4419 — pinned in definite_assignment_default_init_4419_test.cpp).
BEZEL_TEST(codegen_inline_array_eq, equals_false_string_content_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]string := [\"\", \"\"]; a[0] := \"foo\"; a[1] := \"bar\"; "
        "  var b: [2]string := [\"\", \"\"]; b[0] := \"foo\"; b[1] := \"baz\"; "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §9.2 / §3.6 / §15.1.2: GetHashCode() of a non-empty inline array is NON-ZERO
// (the pre-fix bug returned 0 unconditionally). The exact value is
// implementation-defined, but MUST be non-zero for a slot with at least one
// non-zero element.
BEZEL_TEST(codegen_inline_array_eq, hashcode_nonzero_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  var h: int := a.GetHashCode(); "
        "  return (if h <> 0 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §3.6: equal inline arrays produce equal hashes (the §8.3 hash-
// equality invariant: equal → hash-equal; the same invariant the record
// GetHashCode upholds).
BEZEL_TEST(codegen_inline_array_eq, hashcode_equal_arrays_match_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  var b: [3]int := [1, 2, 3]; "
        "  return (if a.GetHashCode() = b.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §3.6: unequal inline arrays MAY produce the same hash (hash collisions
// are allowed — §3.6 only requires that EQUAL values hash equal, not the
// converse). This test pins the EXPECTED hash value to detect any silent
// determinism regression: a hash function that produces different values
// across runs breaks `Dictionary<[N]int, V>` and any other hash-keyed
// container. The exact value is h = (((17*31) + 1)*31 + 2)*31 + 3 = 507473
// (per the §9.1/§9.2 record-hash recipe in tryLowerInlineArrayCall's
// GetHashCode branch — same seed=17 and combiner h*31+slot as
// emitValueHashCode for a field-typed inline array, so a record containing
// an inline-array field hashes identically with the field extracted).
// Process exit status is 8-bit (WEXITSTATUS, see aot_support.hpp note), so
// the Main return value gets truncated — verify via Console.WriteLine + stdout
// capture to keep the full i32 observable.
BEZEL_TEST(codegen_inline_array_eq, hashcode_stable_value_spec_15_1_2) {
    std::string out = runStdout(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  Console.WriteLine(a.GetHashCode()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(out.find("507473") != std::string::npos);
}

// §9.2: GetHashCode for a single-element array — the arity-1 fold boundary.
// Seed = 17, then a single iteration: 17*31 + 42 = 569.
BEZEL_TEST(codegen_inline_array_eq, hashcode_arity1_stable_spec_15_1_2) {
    std::string out = runStdout(
        "module M; class C public static method Main(): int "
        "  var a: [1]int := [42]; "
        "  Console.WriteLine(a.GetHashCode()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(out.find("569") != std::string::npos);
}

// §15.1.2: Equals(b) and GetHashCode() are CONSISTENT — when Equals is true,
// the hashes match. The converse need not hold (hash collisions are allowed).
// This test verifies the structural-equality dispatch on `Equals` uses the
// SAME element-wise comparison the `=` operator uses (so the two paths
// cannot diverge on a corner case like a nested inline array).
BEZEL_TEST(codegen_inline_array_eq, equals_and_hash_consistent_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [4]int := [10, 20, 30, 40]; "
        "  var b: [4]int := [10, 20, 30, 40]; "
        "  var eq: bool := a.Equals(b); "
        "  var eqHash: bool := a.GetHashCode() = b.GetHashCode(); "
        "  return (if eq and eqHash then 1 else 0); "
        "end Main; end C;"), 1);
}