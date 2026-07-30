// Codegen — issue #2398 / spec §9.8 (serialization synthesis, incl. the
// record-admissible clause) + §28.3/§28.9 (exactly-once reclamation), docs
// revision v0.235.
//
// #2378 fixed emitClassDeserialize's Bug B: a component slot already
// populated by an EARLIER successful field read had no disposal registered
// anywhere, so a LATER field's read that throws orphaned the already-read
// value. #2398 is the SAME gap in emitRecordDeserialize and
// emitChoiceDeserialize — Bug A (emitDeserializeField's missing
// emitPendingCheck) is shared codegen and was already fixed by #2378; Bug
// B's fix (the dedicated disposeStack_ scope) was class-only.
//
// VERIFY-FIRST findings (scripts/agent_check.sh --probe --asan, against
// pre-fix main @ 9468946):
//
//   * The issue's own record repro (`Pair(first: string, second: int)`,
//     cutLen = bytes.Length - 3) reproduces EXACTLY: "63 byte(s) leaked in
//     2 allocation(s)" (a 32-byte bz_string header + a 31-byte UTF-8 data
//     buffer for First's 30-character value) — identical to #2378's own
//     string repro.
//   * The identical shape for a choice (`Both(first: string, second: int)`
//     payload) leaks the identical 63 bytes / 2 allocations.
//   * A record with THREE fields (two string, one int) — `Triple(a, b:
//     string, c: int)`, same cutLen pattern — leaks BOTH string siblings:
//     "111 byte(s) leaked in 4 allocation(s)".
//   * Two levels of record nesting (`Pair(first: string, nested: Leaf(tag:
//     string, v: int))`, cut inside Leaf's own trailing `v`) leaks BOTH
//     Pair.first (the outer sibling) AND Leaf.tag (read successfully
//     inside the nested recursive Deserialize call) — "110 byte(s) leaked
//     in 4 allocation(s)" — proving the disposal gap exists at every
//     recursion depth, not just the outermost frame.
//   * A record whose component is itself a FULLY, successfully read nested
//     record with its own string field (`Pair(nested: Leaf(tag: string),
//     second: int)`, Leaf itself never truncated) ALSO leaks when the
//     LATER `second` throws: "55 byte(s) leaked in 2 allocation(s)" (Leaf's
//     own bz_string header + data). This is a slightly different shape
//     from the issue's own "already-read SIBLING component" framing — the
//     already-read component here is a whole nested VALUE, not a bare
//     string — but it is the same root cause (no disposal registered for
//     an already-populated slot) and is fixed by the same mechanism, reusing
//     the existing #1747 `emitRecordStringFieldReleases` one-level release
//     helper rather than inventing new machinery.
//
// Component-type survey (§9.8's record-admissible clause: "a primitive,
// string, an enum, another serialization-eligible record, a value-like
// serialization-eligible choice ..., T? of a record-admissible T, or a
// non-owning inline array ... of record-admissible element type"):
//   * primitives/enums: no heap ownership, nothing to leak.
//   * string / string?: the direct #2398 fix (stringRelease disposal,
//     mirroring #2378's class-side registration exactly — string? uses the
//     same bare-nullable-pointer representation as string, so no separate
//     nullable check is needed, matching the class fix's own condition).
//   * nested record (non-nullable): fixed via the #1747
//     `emitRecordStringFieldReleases` reuse (isRecordWithStringFieldsTypeRef
//     already excludes a nullable record component, whose slot is the
//     wrapped {i8, T} struct, not the bare layout that helper expects).
//   * value-like choice component, and inline `[N]string`/`[N]RecordType`
//     array components: NOT covered by this fix (no equivalent release
//     helper exists yet) — a residual, documented gap; see #2402.
//   * A record/choice component that is itself a record TWO OR MORE levels
//     of nesting deep (a nested record whose own nested-record field owns
//     a string) is NOT fully covered — the reused #1747 helper is only
//     ONE level deep, matching its own established scope. Confirmed
//     leaking via probe (`Root(mid: Mid(leaf: Leaf(tag: string)), second:
//     int)`, cut inside `second`: "60 byte(s) leaked in 2 allocation(s)")
//     — filed as #2401 (a residual, narrower version of the same bug this
//     fix addresses one level deep).
//
// Two SEPARATE, more severe bugs were discovered while designing test cases
// and are explicitly NOT fixed here (different root cause, different fix
// site, filed separately per the project's decline-with-reasons discipline):
//   * #2400: a truncated `string` VALUE (not a structural field/name/count)
//     in the BINARY format never raises SerializationException at all —
//     bz_binder_read_bz_string (and its bz_binder_read_string /
//     bz_binder_read_field_name callers) return null on truncation WITHOUT
//     calling bz_ser_throw, unlike bz_binder_read_int/_uint/_float/etc,
//     which always throw. The record/choice deserializer loop does not
//     check for a null field VALUE either, so a truncated trailing string
//     silently "succeeds" with an incomplete/absent value instead of
//     raising — confirmed via probe (a truncation landing inside a second
//     `string` component's own length-prefix or data bytes returns exit 7,
//     i.e. reaches the success return, for every cut in [1, 20] against a
//     77-byte two-string-component wire). This is why the tests below use
//     an `int` (or a nested type whose own `bz_binder_begin_object` reads
//     a length-prefixed count, which DOES throw reliably) as the field
//     whose truncation drives the exception, rather than truncating
//     directly inside a trailing string value.
//   * #2400 (same repro family): emitRecordDeserialize / emitChoiceDeserialize
//     never null-check the `bz_binder_read_field_name` return before
//     dereferencing it (unlike emitClassDeserialize's explicit `errBB`
//     null check) — a truncation landing inside a field NAME's own
//     length-prefix or bytes SEGFAULTs instead of raising. Confirmed via
//     probe: cuts >= 24 against the same 77-byte wire (landing inside the
//     second field's own name bytes) crash with SIGSEGV.
//
// FAILS WITHOUT / PASSES WITH: every runAotChecked test below whose comment
// names a leak size flips r.ran from false to true (confirmed via
// scratch-copy `git diff` of src/codegen/LlvmBackend_Serialization.cpp
// applied/reverted with `git apply`/`git checkout` — NOT git stash — +
// rebuild + re-run, then restored). See the fix commit message for the
// full mechanical-proof transcript.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Preamble shared by every test: BinarySerializer/BinaryDeserializer require
// `import System.Serialization` (stdlib §2.15).
const char* kImport = "import System.Serialization; ";
}  // namespace

// ===========================================================================
// The issue's own exact repro: a record's already-read `string` sibling
// leaks when a LATER `int` field's read throws.
// ===========================================================================
BEZEL_TEST(serialization_sibling_leak_2398,
           record_string_sibling_orphaned_by_int_truncation_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: int): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 63-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// The identical shape for a value-like CHOICE: an already-read `string`
// payload sibling leaks when the case's LATER `int` payload throws.
// ===========================================================================
BEZEL_TEST(serialization_sibling_leak_2398,
           choice_string_payload_sibling_orphaned_by_int_truncation_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Both(first: string, second: int), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Both(\"a-fairly-long-tag-value-string\", 12345); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 63-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// "Record with two string components truncated after the first": BOTH
// already-read string siblings (`a`, `b`) leak, not just one, when the
// trailing `int` field `c` throws. (A truncation landing directly inside a
// trailing STRING value never raises at all in the binary format — #2400 —
// so `c: int` is what reliably drives the exception here; the point under
// test is that MULTIPLE already-read string slots are all disposed, not
// just the first one populated.)
// ===========================================================================
BEZEL_TEST(serialization_sibling_leak_2398,
           record_two_string_siblings_both_orphaned_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Triple(a: string, b: string, c: int): ISerializable, IDeserializable "
        "end Triple; "
        "class C public static method Main(): int "
        "  var p: Triple := new Triple(\"first-tag-value-string\", "
        "                              \"second-tag-value-string\", 999); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Triple := Triple.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 111-byte / 4-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// A nested eligible record failing inside its OWN Deserialize: the outer
// record's `first` (already read) AND the nested record's OWN `tag`
// (already read, inside the recursive call) both leak when the nested
// record's trailing `v: int` throws. Proves the fix protects every
// recursion depth (the SAME emitRecordDeserialize function, called
// recursively, pushes its own fresh scope each invocation) — mirrors
// #2378's "two levels deep" class test.
// ===========================================================================
BEZEL_TEST(serialization_sibling_leak_2398,
           record_nested_eligible_record_own_deserialize_failure_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string, v: int): ISerializable, IDeserializable "
        "end Leaf; "
        "record Pair(first: string, nested: Leaf): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"outer-tag-value-string\", "
        "                          new Leaf(\"inner-tag-value-string\", 9)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 110-byte / 4-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Extension beyond the issue's literal ask, same root cause: a component
// that is itself a FULLY, successfully read nested record (its own read
// never truncated) still owns a string the outer scope must dispose when a
// LATER sibling throws. Fixed via the #1747 `emitRecordStringFieldReleases`
// one-level reuse (see the file header's component-type survey).
// ===========================================================================
BEZEL_TEST(serialization_sibling_leak_2398,
           record_fully_read_nested_record_component_orphaned_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string): ISerializable, IDeserializable end Leaf; "
        "record Pair(nested: Leaf, second: int): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(new Leaf(\"inner-tag-value-string\"), 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 55-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Clean, full-read controls (no truncation): round-trips correctly AND the
// fix's new per-slot disposal registration -- popped WITHOUT running its
// disposals once construction begins -- does not itself leak or double-free
// on the ordinary success path.
// ===========================================================================
// NOTE: intentionally a FLAT (string, int) shape, not a nested-record
// component -- a `var back := Pair.Deserialize(d)` binding a record whose
// OWN direct string field(s) are eligible for the #1747
// recordCallResultReleaseEligible scope-end release, but a NESTED record
// component's own string field is not (that mechanism, like this fix's
// reused emitRecordStringFieldReleases, is one level deep) -- leaks on
// this success path REGARDLESS of this fix (confirmed via probe: the
// nested-record shape leaks 55 bytes here on both pre-fix and post-fix
// main, an unrelated pre-existing gap in the #1747 caller-side mechanism,
// not emitRecordDeserialize/emitChoiceDeserialize -- filed as #2401,
// folded with the two-level-nesting follow-up). Keeping this control flat
// isolates it to exactly what THIS fix touches: the exception/unwind path
// inside the deserializer itself.
BEZEL_TEST(serialization_sibling_leak_2398,
           clean_full_read_record_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: int): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: Pair := Pair.Deserialize(d); "
        "  if back.first <> \"a-fairly-long-tag-value-string\" then return 1; end; "
        "  if back.second <> 12345 then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(serialization_sibling_leak_2398,
           clean_full_read_choice_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Both(first: string, second: int), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Both(\"a-fairly-long-tag-value-string\", 12345); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of "
        "    Both(var f, var sec) when f = \"a-fairly-long-tag-value-string\" "
        "                             and sec = 12345 => 42, "
        "    _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Adversarial self-pass: a MISSING required component (a structural fault
// via the recSeenFlags check, not a truncated read) also disposes an
// already-read prior sibling without leaking OR double-freeing — this path
// (`emitThrowBuiltin` from the missing-required-component check) is now
// covered too, since the disposal scope's pop was moved to AFTER that check
// (previously it wasn't a disposal boundary at all; the pre-#2398 comment
// there explicitly "accepted the minor leak").  JsonDeserializer is used to
// hand-construct an object literally missing the second field.
// ===========================================================================
BEZEL_TEST(serialization_sibling_leak_2398,
           record_missing_required_component_after_string_sibling_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: int): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer("
        "      \"{\\\"first\\\":\\\"a-fairly-long-tag-value-string\\\"}\"); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // a leak or double-free here would flip r.ran false
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Adversarial self-pass (double-free): does fixing the leak introduce a
// DOUBLE-FREE on the path where an already-CONSUMED (fully constructed,
// transferred into the result) nested record value -- itself owning a
// string field -- is followed by a later sibling field that throws? This is
// the exact "fully read nested record component" shape above, re-asserted
// here under its own self-pass name per the project's discipline of
// pinning every self-pass axis as its own permanent regression, not just a
// spot-check.
// ===========================================================================
BEZEL_TEST(serialization_sibling_leak_2398,
           nested_record_string_field_not_double_disposed_spec_28_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string): ISerializable, IDeserializable end Leaf; "
        "record Pair(a: Leaf, b: Leaf, c: int): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(new Leaf(\"leaf-a-tag-value\"), "
        "                          new Leaf(\"leaf-b-tag-value\"), 77); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        // Both `a` and `b` (each a fully-constructed Leaf owning a string
        // field) are read successfully; the cut lands inside `c`'s own int
        // value.
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // a double-free/UAF here would flip r.ran false
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Adversarial self-pass (double-free), second angle: THREE plain string
// siblings, all successfully read, with the LAST one's read throwing via a
// nested type whose own truncated bz_binder_begin_object throws (not a
// bare scalar) -- verifies the automatic sweep handles a mix of plain
// string slots when the throw itself originates from deep inside a nested
// recursive call, not just this function's own scalar read.
BEZEL_TEST(serialization_sibling_leak_2398,
           three_string_siblings_then_nested_record_truncation_not_double_disposed_spec_28_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(v: int): ISerializable, IDeserializable end Leaf; "
        "record Quad(a: string, b: string, c: string, nested: Leaf): "
        "    ISerializable, IDeserializable end Quad; "
        "class C public static method Main(): int "
        "  var p: Quad := new Quad(\"tag-a-value\", \"tag-b-value\", \"tag-c-value\", "
        "                          new Leaf(5)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        // Cut lands inside Leaf's own trailing int value (the very last
        // field of the very last nested structure, so the last few bytes
        // of the whole wire).
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Quad := Quad.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // a double-free/UAF here would flip r.ran false
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// run() sanity: confirm the exact-repro program (no ASan) also produces the
// expected exit code via the plain runAot path, matching this file's other
// suites' dual-harness convention.
BEZEL_TEST(serialization_sibling_leak_2398,
           record_string_sibling_orphaned_plain_exit_code_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: int): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);
}
