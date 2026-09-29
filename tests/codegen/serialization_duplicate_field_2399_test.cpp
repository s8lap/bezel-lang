// Codegen — issue #2399 / spec §9.8 (serialization synthesis) + stdlib §2.15
// (concrete serialization formats) + §28.3/§28.9 (exactly-once reclamation),
// docs revision v0.235.
//
// SPEC READING (the design question the issue asks to settle): language spec
// §9.8, "Construct the result" paragraph: "Field order in the input need not
// match declaration order -- dispatch is by field name. A missing required
// component (one neither present nor `serialized optional`), a duplicate
// field name, a type mismatch, or a name not matching any enum constant
// raises SerializationException unconditionally -- these are structural
// faults in the input regardless of policy." Stdlib §2.15, Unknown-field
// policy paragraph: "This policy governs only unknown fields; missing
// required fields, duplicates, and type mismatches remain unconditional
// errors regardless of the setting". Both texts are explicit and agree: a
// duplicate wire field name is MALFORMED INPUT that must raise
// SerializationException -- "last one wins" is not a legal reading. This
// file implements and pins that reading (not the alternative "release the
// displaced value and keep going" reading the task also floated) -- the
// spec decides the question, so no issue was needed for the choice itself.
//
// VERIFY-FIRST findings (scripts/agent_check.sh --probe --asan, against
// pre-fix main):
//   * The issue's own class repro (`Pair(first: string, second: int)`, JSON
//     `{"First": "aaa...a", "First": "bbb...b", "Second": 5}`, 40-char
//     values): prints the SECOND value ("last one wins") and returns 0
//     (should raise) -- "73 byte(s) leaked in 2 allocation(s)" under LSan
//     (a 32-byte bz_string header + a 41-byte UTF-8 data buffer for the
//     orphaned FIRST value), matching the issue's own report exactly.
//   * The identical shape for a RECORD (`record Pair(first, second)`): same
//     73 bytes / 2 allocations, same last-wins behavior.
//   * The identical shape for a value-like CHOICE payload (`Both(first,
//     second)`, discriminated by `"$case"`): same 73 bytes / 2 allocations
//     (a JSON-parsing quirk unrelated to this issue was found and worked
//     around while building this repro -- see "ADJACENT FINDING" below).
//   * A duplicate OWNED NESTED CLASS component (`Outer(owned a: Leaf, b:
//     int)`, `Leaf(tag: string)`): "96 byte(s) leaked in 3 allocation(s)"
//     (the ENTIRE first Leaf object -- its own 24-byte struct AND its owned
//     Tag string's 40+32-byte header+data -- orphaned, not just a bare
//     string), confirming the leak matrix extends past plain strings to any
//     owned heap component.
//   * A TRIPLICATE (three occurrences of the same field name): "146 byte(s)
//     leaked in 4 allocation(s)" -- BOTH the first ("aaa...") and second
//     ("bbb...") occurrences orphaned while the third ("ccc...") wins,
//     proving the leak is cumulative across every superseded occurrence, not
//     just the first->second transition.
//   * A duplicate `serialized optional` field (`Extended(V: int, Note:
//     string? serialized optional)`): leaks and last-wins identically to a
//     required field -- optionality is about a MISSING occurrence, not a
//     DUPLICATE one; §9.8/§2.15's "unconditionally"/"regardless of the
//     setting" language covers both required and optional components alike.
//   * A clean (no-duplicate) control round-trips correctly with no leak,
//     confirming the matrix isn't an artifact of the harness.
//
// FIX: emitClassDeserialize / emitRecordDeserialize / emitChoiceDeserialize
// (LlvmBackend_Serialization.cpp) each already track a per-component
// "seen" flag for the fix #668 missing-required-field check (previously
// allocated ONLY for required, i.e. non-`serialized optional`, components).
// This fix (a) allocates that flag for EVERY non-skip component, required
// or optional, and (b) checks it at the TOP of the per-field dispatch's
// matched branch, BEFORE reading anything for this occurrence: if already
// seen, raise SerializationException immediately rather than reading the
// new value and overwriting the slot. Because the earlier occurrence's
// value is already sitting in slots[]/slots[pi] and is registered in the
// existing #2378/#2398 disposeStack_ scope (guarding exactly this kind of
// already-populated-slot-orphaned-by-a-later-throw shape), raising here lets
// that SAME unwind sweep dispose the superseded value -- no new disposal
// mechanism was needed, just an earlier trigger point for the existing one.
//
// ADVERSARIAL SELF-PASS FINDING (regression caught by this file's own
// development, NOT shipped): the class-side seen-flags were originally
// zero-initialized in the FUNCTION's entry block (mirroring namePtrSlot's
// fix #669 pattern, itself needed because namePtrSlot is swept by a
// disposal path that can run before this invocation's own code has), which
// runs ONCE PER FUNCTION rather than once per logical Deserialize call. A
// class's synthesized Deserialize is inline-emitted at its call site (not a
// standalone function), so calling it repeatedly inside a loop -- exactly
// the existing `class_primary_ctor_serialization_2325_test.cpp`
// `repeated_roundtrip_no_accumulating_leak_spec_9_8` shape -- reuses the
// SAME allocas on every iteration. With the entry-block zero-init, a flag
// set to 1 on iteration 1 stayed 1 forever after, so iteration 2's very
// first (non-duplicate!) field read looked like a duplicate of iteration
// 1's already-consumed-and-freed value: a false-positive throw whose unwind
// then bz_string_release'd a STALE pointer left over from iteration 1 -- a
// use-after-free SEGV. Fixed by zero-initializing at the CURRENT insertion
// point instead (matching emitRecordDeserialize's and
// emitChoiceDeserialize's seenFlags loops, which never had this bug -- they
// already used a plain builder_.CreateStore here, not the entry-block
// trick). The `class_repeated_deserialize_loop_no_stale_false_positive`
// test below pins this specific shape directly (repeated calls, each with
// its OWN genuine duplicate, must ALL raise -- not just the first).
//
// ADJACENT FINDING (filed as its own issue, NOT fixed here): building the
// choice-payload duplicate repro above first hit an UNRELATED pre-existing
// bug -- `bz_binder_read_case_discriminator`'s JSON path never skips
// whitespace between the "$case" field's colon and its value, so ordinary
// (spaced) JSON like `{"$case": "Both", ...}` spuriously raises
// SerializationException even with no duplicate present. Isolated with a
// before/after whitespace-only control, confirmed as a distinct bug, and
// filed separately (issue #2419) per the project's decline-with-reasons
// discipline; this file's choice tests use unspaced `"$case":"Both"` JSON to
// avoid the confound (every other field in these tests uses ordinary spaced
// JSON, which works correctly on every path this file actually touches).
//
// NOT this issue's territory (confirmed via probe, explicitly out of
// scope per the task brief): #2405 (residual leak at 2+ levels of record
// nesting -- unrelated slot-disposal gap, not a duplicate-name issue).
// EMPIRICALLY CONFIRMED INTERACTION (probe, not shipped as a test here --
// #2405's own fix is the right place to pin it): a duplicate of a component
// whose value is itself TWO levels of record nesting deep (`Root(mid: Mid,
// second: int)`, `Mid(leaf: Leaf)`, `Leaf(tag: string)`, the FIRST `"mid"`
// occurrence's Leaf.tag ~48 chars) DOES correctly raise
// SerializationException (confirmed exit 42 with ASAN_OPTIONS=detect_leaks=0
// to read the true exit status past LSan's exit-code override) -- this
// fix's duplicate-name CHECK is unconditional and fires regardless of
// nesting depth -- but the superseded first occurrence's deeply-nested
// string is NOT disposed on that unwind ("80 byte(s) leaked in 2
// allocation(s)" under LSan): the #2398/#2405 disposal scope's reused
// #1747 `emitRecordStringFieldReleases` helper is one level deep by design,
// so it does not reach Mid->Leaf->Tag. This fix inherits that PRE-EXISTING
// gap rather than introducing a new one -- the correctness half (raise) is
// unconditional and unaffected; the disposal-completeness half is bounded
// by whatever depth #2405's own eventual fix reaches.
// #2397 (the #2378 Bug-B disposal scope excludes graph-mode classes --
// this fix's duplicate-name CHECK still correctly raises for a graph-mode
// component too, since it runs before any graph-mode-specific marker/tag
// read, but the SUPERSEDED value's disposal in graph mode inherits #2397's
// pre-existing gap, not a new one this fix introduces).
//
// FAILS WITHOUT / PASSES WITH: every runAotChecked test below whose comment
// names a leak size, or whose scenario is "should raise but doesn't", flips
// r.ran/r.exitCode from the wrong (pre-fix) value to the expected one
// (confirmed via scratch-copy `git diff` of
// src/codegen/LlvmBackend_Serialization.cpp saved to a patch file, `git
// checkout` reverted, rebuilt + re-probed to confirm the pre-fix failure
// mode, then `git apply` restored -- NEVER `git stash`, per project
// discipline). See the fix commit message for the full mechanical-proof
// transcript and before/after byte counts.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// runAotChecked returns ran=false on any sanitizer fire (leak / UAF / double-
// free); map that to a sentinel so a single EXPECT_EQ pins BOTH memory-
// cleanliness AND the round-tripped/exception-caught value.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// Preamble shared by every test: JsonDeserializer requires
// `import System.Serialization` (stdlib §2.15).
const char* kImport = "import System.Serialization; ";

}  // namespace

// ===========================================================================
// The issue's own exact repro: a CLASS's duplicate `string` field name must
// raise, not silently overwrite-and-leak.
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           class_duplicate_string_field_raises_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Pair(first: string, second: int): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public Second: int := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"First\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"First\\\": "
        "\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\", \\\"Second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);   // 0 pre-fix (last-wins, no raise) + 73-byte/2-alloc leak
}

// Sanity: the same repro also raises under the plain (non-ASan) runAot path.
BEZEL_TEST(serialization_duplicate_field_2399,
           class_duplicate_string_field_raises_plain_exit_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Pair(first: string, second: int): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public Second: int := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"First\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"First\\\": "
        "\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\", \\\"Second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);
}

// ===========================================================================
// A duplicate OWNED NESTED CLASS component: not just a bare string leaks --
// the whole superseded Leaf object (its own allocation AND its owned Tag
// string) must be disposed exactly once.
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           class_duplicate_owned_nested_component_raises_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Leaf : ISerializable, IDeserializable "
        "  public property Tag: string; "
        "  public constructor(tag: string) Tag := tag; end constructor; "
        "end Leaf; "
        "class Outer : ISerializable, IDeserializable "
        "  public owned A: Leaf; "
        "  public property B: int; "
        "  public constructor(owned a: Leaf, b: int) "
        "    A := a; B := b; "
        "  end constructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"A\\\": {\\\"Tag\\\": "
        "\\\"leaf-one-tag-value-aaaaaaaaaaaaaaaaaaaa\\\"}, \\\"A\\\": "
        "{\\\"Tag\\\": \\\"leaf-two-tag-value-bbbbbbbbbbbbbbbbbbbb\\\"}, "
        "\\\"B\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Outer.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);   // 0 pre-fix (last-wins) + 96-byte/3-alloc leak (whole Leaf)
}

// ===========================================================================
// The identical shape for a RECORD: a duplicate field name must raise, not
// overwrite-and-leak.
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           record_duplicate_string_field_raises_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: int): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"first\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"first\\\": "
        "\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\", \\\"second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);   // 0 pre-fix (last-wins) + 73-byte/2-alloc leak
}

// ===========================================================================
// The identical shape for a value-like CHOICE payload field. (Uses unspaced
// `"$case":"Both"` JSON to sidestep the unrelated #2419 whitespace bug --
// see the file header's ADJACENT FINDING.)
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           choice_duplicate_payload_string_field_raises_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Both(first: string, second: int), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$case\\\":\\\"Both\\\", \\\"first\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"first\\\": "
        "\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\", \\\"second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);   // 0 pre-fix (last-wins) + 73-byte/2-alloc leak
}

// ===========================================================================
// A TRIPLICATE (three occurrences of the same field name): the SECOND
// occurrence must raise as soon as it re-matches the first -- proving the
// duplicate check fires on the SECOND occurrence specifically (not merely
// "more than two"), and that both superseded values in a longer run would be
// disposed were the third occurrence ever reached (it is not, since the
// second occurrence already raises).
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           class_triplicate_field_raises_at_second_occurrence_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Pair(first: string, second: int): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public Second: int := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"First\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"First\\\": "
        "\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\", \\\"First\\\": "
        "\\\"cccccccccccccccccccccccccccccccccccccccc\\\", \\\"Second\\\": "
        "5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);   // 0 pre-fix (third wins) + 146-byte/4-alloc leak (BOTH a and b orphaned)
}

// ===========================================================================
// A duplicate `serialized optional` field: optionality is about a MISSING
// occurrence (default(T)), not a duplicate one -- §9.8/§2.15's
// "unconditionally"/"regardless of the setting" language names duplicates
// alongside missing-required and type-mismatch, with no optional carve-out.
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           class_duplicate_optional_field_raises_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Extended : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public property Note: string? serialized optional; "
        "  public constructor(v: int, note: string?) "
        "    V := v; Note := note; "
        "  end constructor; "
        "end Extended; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"V\\\": 99, \\\"Note\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"Note\\\": "
        "\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\"}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Extended.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);   // 0 pre-fix (last-wins for optional fields too) + leak
}

// ===========================================================================
// Self-pass (a): DOUBLE-FREE -- a class with TWO owned nested components
// (A, B), each read exactly once successfully, followed by a DUPLICATE of B.
// Must dispose ONLY B's superseded value, leaving A (read once, never
// duplicated) untouched by the same sweep -- i.e. the disposal scope's sweep
// must not double-dispose an unrelated sibling that was never re-matched.
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           class_duplicate_of_one_sibling_does_not_touch_other_sibling_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Leaf : ISerializable, IDeserializable "
        "  public property Tag: string; "
        "  public constructor(tag: string) Tag := tag; end constructor; "
        "end Leaf; "
        "class Pair : ISerializable, IDeserializable "
        "  public owned A: Leaf; "
        "  public owned B: Leaf; "
        "  public constructor(owned a: Leaf, owned b: Leaf) "
        "    A := a; B := b; "
        "  end constructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"A\\\": {\\\"Tag\\\": "
        "\\\"leaf-a-tag-value-aaaaaaaaaaaaaaaaaaaa\\\"}, \\\"B\\\": "
        "{\\\"Tag\\\": \\\"leaf-b-tag-value-bbbbbbbbbbbbbbbbbbbb\\\"}, "
        "\\\"B\\\": {\\\"Tag\\\": "
        "\\\"leaf-b2-tag-value-cccccccccccccccccccc\\\"}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);   // a double-free/UAF touching A here would flip r.ran false
}

// ===========================================================================
// Self-pass (b) / adversarial regression: repeated Deserialize CALLS in a
// LOOP, each with its OWN genuine duplicate field, must ALL raise --
// pinning the loop-staleness bug found and fixed while developing this fix
// (see the file header's ADVERSARIAL SELF-PASS FINDING). A class's
// synthesized Deserialize is inline-emitted at its call site, so the
// seen-flags this fix introduces must reset EVERY iteration, not just once
// per enclosing function -- with the entry-block-forced zero-init bug this
// test's SECOND iteration onward would either wrongly fail to raise (a
// stale-1 flag looking like "already seen" for a field that hasn't been
// read yet THIS iteration -- but here every field genuinely IS duplicated,
// so that particular symptom wouldn't surface) or, as was actually
// observed, crash with a use-after-free when the false-positive throw
// disposes an already-freed prior-iteration slot.
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           class_repeated_deserialize_loop_no_stale_false_positive_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Pair(first: string, second: int): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public Second: int := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var caughtCount: int := 0; "
        "  var i: int := 0; "
        "  while i < 10 do "
        "    var json: string := \"{\\\"First\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"First\\\": "
        "\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\", \\\"Second\\\": "
        "5}\"; "
        "    using var d := new JsonDeserializer(json); "
        "    try "
        "      using var back := Pair.Deserialize(d); "
        "    catch SerializationException => caughtCount := caughtCount + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return (if caughtCount = 10 then 42 else caughtCount); "
        "end Main; end C;"),
        42);   // pre-fix: SEGV (use-after-free) from iteration 2 onward
}

// ===========================================================================
// Self-pass (c): a MISSING required component (a structural fault via the
// seen-flag missing-field check, not a duplicate) still raises correctly --
// confirms the `required` gate this fix added to that check (skipping
// entries for `serialized optional` components, which this fix now also
// tracks via the SAME seen-flag vector) did not accidentally suppress the
// pre-existing missing-required-field diagnostic (fix #668).
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           class_missing_required_field_still_raises_no_regression_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Pair(first: string, second: int): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public Second: int := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"First\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\"}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);
}

// ===========================================================================
// Self-pass (d): a MISSING `serialized optional` field (no duplicate, just
// genuinely absent) still silently defaults rather than raising -- confirms
// allocating a seen-flag for optional components too (needed so the
// duplicate check can see them) did not accidentally make them REQUIRED.
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           class_missing_optional_field_still_defaults_no_regression_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Extended : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public property Note: string? serialized optional; "
        "  public constructor(v: int, note: string?) "
        "    V := v; Note := note; "
        "  end constructor; "
        "end Extended; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"V\\\": 99}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  using var back := Extended.Deserialize(d); "
        "  if back.V <> 99 then return 91; end; "
        "  return (if back.Note = null then 42 else 92); "
        "end Main; end C;"),
        42);
}

// ===========================================================================
// Clean, full-read controls (no duplicate anywhere): every shape round-trips
// correctly AND this fix's new per-field seen-flag check does not itself
// leak or misfire on the ordinary, well-formed path.
// ===========================================================================
BEZEL_TEST(serialization_duplicate_field_2399,
           clean_no_duplicate_class_roundtrip_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Pair(first: string, second: int): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public Second: int := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"First\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"Second\\\": "
        "5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  using var back := Pair.Deserialize(d); "
        "  if back.First <> \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" "
        "  then return 1; end; "
        "  if back.Second <> 5 then return 2; end; "
        "  return 42; "
        "end Main; end C;"),
        42);
}

BEZEL_TEST(serialization_duplicate_field_2399,
           clean_no_duplicate_record_roundtrip_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: int): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"first\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"second\\\": "
        "5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var back: Pair := Pair.Deserialize(d); "
        "  if back.first <> \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" "
        "  then return 1; end; "
        "  if back.second <> 5 then return 2; end; "
        "  return 42; "
        "end Main; end C;"),
        42);
}

BEZEL_TEST(serialization_duplicate_field_2399,
           clean_no_duplicate_choice_roundtrip_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Both(first: string, second: int), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$case\\\":\\\"Both\\\", \\\"first\\\": "
        "\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\", \\\"second\\\": "
        "5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of "
        "    Both(var f, var sec) "
        "      when f = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" "
        "        and sec = 5 => 42, "
        "    _ => 0); "
        "end Main; end C;"),
        42);
}
