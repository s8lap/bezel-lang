// Codegen — fix #3798 / stdlib §4.8 / §28.9 (v0.286): `Dictionary<K, V>` /
// `SortedDictionary<K, V>` whose V is a non-nullable record / choice with a
// `string` field (directly or transitively through a non-nullable nested
// record / choice payload) leaked that nested string allocation when the
// entry was freed by `.Remove(key)`, `.Clear()`, or scope-end Dispose. The
// `valueBoxFreeThunk` was — and `dictValueBoxFreer`'s `.Remove` fallback
// (#3787) still is — a plain `free(box)` thunk. `free(box)` reclaims the
// box's malloc'd struct storage but does NOT walk the V's nested fields, so
// a `Box { S: string }` value's `S` +1 was orphaned every time.
//
// ROOT CAUSE (`LlvmBackend_Disposal.cpp`'s `valueBoxFreeThunk`, ~L4411):
// the boxed V is a value type whose struct base is the box pointer; the
// box-freer is type-erased `void(void*)`; `free(box)` ignores every field.
// `dictValueBoxFreer` returned the same thunk for every boxed-struct V
// (record, nullable value type, choice) regardless of whether V owns a
// string. A record/choice WITHOUT string fields is correct (free is
// sufficient); a record/choice WITH string fields is not.
//
// FIX (`LlvmBackend_Disposal.cpp`'s `dictValueBoxFreer`): dispatch on V's
// kind + `recordHasStringFields` / `choiceHasStringFields`:
//   * V = non-nullable record with owned strings → `boxedRecordValueFreer(valTy)`
//   * V = non-nullable choice with owned strings → `boxedChoiceValueFreer(valTy)`
//   * otherwise → `valueBoxFreeThunk()` UNCHANGED
//
// The two new helpers build per-V `void(void*)` thunks that walk V's
// string fields (reusing `emitRecordStringFieldReleases_impl` #1747/#2405
// and `emitChoiceStringFieldReleases_impl` #2425 — the SAME walkers
// scope-end Disposal already uses for plain `var` record / choice locals)
// and then `free(box)`s the storage. Cached per-V TypeRef.
//
// §6b — DOUBLE-FREE ANALYSIS.
//   * Non-matching V (primitive / scalar / non-string-owning record /
//     nullable V / generic V) keeps the plain `valueBoxFreeThunk` —
//     UNCHANGED.
//   * Nullable V (`record?`, `choice?`) is intentionally excluded: the
//     box's layout is `{i8, T}`, not the bare T the walker expects. Same
//     Nullable-rectypass gap as #2405/#2425 (separately tracked).
//   * Generic V is excluded by the same `recordHasStringFields` /
//     `choiceHasStringFields` non-null-declaration gate.
//   * Remove-then-re-Add allocates a FRESH box on re-Add via `encodeValue`,
//     distinct from the freed one — no double free.
//   * `Clear` / scope-end iterate entries STILL IN the table; the removed
//     / cleared entries' boxes are not revisited.
//
// FAIL-WITHOUT / PASS-WITH (§R12-H): measured on the issue's own probe
// (500 iterations of `Dictionary<int, Box>.Add(k, new Box($"...heap-string..."))`
// then `.Remove(k)`, loop-amplified per [[lsan-one-shot-leak-tests-pass-vacuously]]):
//   * PARENT bezelc (this branch pre-#3798): "44890 byte(s) leaked in
//     1000 allocation(s)" — Direct 16000 B / 500 + Indirect 28890 B / 500,
//     measured directly via `--sanitize=address` (the issue's own
//     verification method), not just via the test harness.
//   * FIX bezelc (this branch + #3798): LSan-clean exit 0.
//
// #3787 RELATIONSHIP: the `dictValueBoxFreer` chain `Remove` already calls
// (#3787's own fallback) is the path that reaches the new per-V freer; #3798
// changes what that fallback RETURNS. The .Remove / .Clear rows below
// exercise the composition: WITHOUT #3787 the box is never freed at all,
// WITHOUT #3798 the box is freed but the nested string is orphaned. Both
// fixes are required for LSan-clean.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repro: record V with a single direct `string` field,
// Dictionary<K, V>, .Add() then .Remove(). 500 iterations under
// runAotChecked (LSan/ASan). FAIL-WITHOUT: 16000 B / 500 direct + 28890 B /
// 500 indirect (the box + the box-stored nested string allocations).
// PASS-WITH: LSan-clean exit.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_box_value_nested_field_freer_3798,
           record_with_string_field_remove_no_leak_4_8) {
    const std::string src =
        "module M; "
        "public record Box "
        "  public S: string; "
        "  public constructor(s: string) this.S := s; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new Dictionary<int, Box>(); "
        "    d.Add(1, new Box($\"iter-{i}-longer-suffix-to-ensure-heap\")); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Same shape with .Clear() instead of .Remove() — same defect, different
// call site. Clear() iterates live entries and disposes them, so the box
// is freed on each Clear — the nested string is the leak. Mirrors the
// §4.8 / §4.17 prose that Clear has the SAME value-disposal contract as
// Remove / Dispose.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_box_value_nested_field_freer_3798,
           record_with_string_field_clear_no_leak_4_8) {
    const std::string src =
        "module M; "
        "public record Box "
        "  public S: string; "
        "  public constructor(s: string) this.S := s; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new Dictionary<int, Box>(); "
        "    d.Add(1, new Box($\"clear-iter-{i}-longer-suffix-heap\")); "
        "    d.Clear(); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// SortedDictionary<K, V> with a string-field record V — same defect as the
// Dictionary rows (dictValueBoxFreer's head-name gate was widened by #3776
// to admit SortedDictionary; this fix's narrower specialization is independent
// of the head name once we ARE inside the boxed-V branch).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_box_value_nested_field_freer_3798,
           sorted_dict_record_string_field_remove_no_leak_4_17) {
    const std::string src =
        "module M; "
        "public record Box "
        "  public S: string; "
        "  public constructor(s: string) this.S := s; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new SortedDictionary<int, Box>(); "
        "    d.Add(1, new Box($\"sorted-iter-{i}-longer-suffix-heap\")); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Choice V with a string payload — same shape #3770's own repro used (Shape
// with two cases, each owning one string field). .Remove() / .Clear() walk
// the box's choiceStruct, locate the active case's payload string, and
// release it. Mirrors the record-V row above but exercises the choice walker.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_box_value_nested_field_freer_3798,
           choice_with_string_payload_remove_no_leak_4_8) {
    const std::string src =
        "module M; "
        "choice Shape "
        "  Circle(s: string), "
        "  Square(s: string); "
        "end Shape; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d.Add(1, Shape.Circle($\"choice-iter-{i}-longer-suffix-heap\")); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Nested record-with-string-field — the box's V carries a non-nullable
// nested record whose own string field is the leak source. Mirrors the
// arbitrary-depth recursion scope-end Disposal already does via
// emitRecordStringFieldReleases_impl #2405; this row exercises it through
// the dict's box-freer path.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_box_value_nested_field_freer_3798,
           nested_record_string_field_remove_no_leak_4_8) {
    const std::string src =
        "module M; "
        "public record Inner "
        "  public T: string; "
        "  public constructor(t: string) this.T := t; end constructor; "
        "end Inner; "
        "public record Outer "
        "  public I: Inner; "
        "  public constructor(i: Inner) this.I := i; end constructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new Dictionary<int, Outer>(); "
        "    d.Add(1, new Outer(new Inner($\"nested-iter-{i}-longer-suffix-heap\"))); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Regression guard: a record V WITHOUT string fields (only int fields) still
// takes the plain `valueBoxFreeThunk` path — UNCHANGED behavior. .Add /
// .Remove / .Dispose with a 2-int record and an explicit Dispose prove the
// non-recursive case stays correct (zero leaks, value checked).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_box_value_nested_field_freer_3798,
           record_without_string_field_unchanged_no_leak_4_8) {
    const std::string src =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Pt>(); "
        "    d.Add(1, new Pt(2, 3)); "
        "    d.Remove(1); "
        "    d.Add(2, new Pt(4, 5)); "
        "    total := total + d[2].X + d[2].Y; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1800"));  // 200 * (4 + 5)
}