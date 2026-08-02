// Codegen — issue #2378 / spec §9.8 (serialization synthesis) + §2.15
// (concrete serialization formats) + §28.3/§28.9 (exactly-once reclamation),
// docs revision v0.235.
//
// VERIFY-FIRST findings (both of the issue's own repros, reproduced verbatim
// below, plus a byte-offset sweep across every length-prefix/name/value
// region of each wire shape — see the fix commit message for the full
// region-by-region table):
//
// Bug A (bz_binder_read_int / read_uint / read_float / read_bool / read_char
// / read_decimal / read_string, all reached via Emitter::emitDeserializeField
// in LlvmBackend_Serialization.cpp): none of these calls were followed by
// emitPendingCheck(), unlike every sibling call site in the same file
// (lowerDeserializerReadMethod's own ReadInt/ReadString/... all check
// immediately). A truncated scalar read sets bz_exc via bz_ser_throw (a
// calloc'd exception object + a bz_make_string message) and returns a
// default value; execution then continued UNCHECKED until a later, unrelated
// call raised a SECOND SerializationException via emitThrowBuiltin, which
// unconditionally overwrites the bz_exc slot with no check for a
// pre-existing pending exception — orphaning (leaking) the first one. Fixed
// by adding emitPendingCheck() after every case in emitDeserializeField's
// primitive switch, matching the already-correct sibling call site.
//
// Bug B (emitClassDeserialize's per-field slots[] array): a component slot
// already populated by an EARLIER successful field read (e.g. a `string`
// field read before an `owned` class field) had no disposal registered
// anywhere. A LATER field's read that recurses into ANOTHER
// emitClassDeserialize invocation and throws correctly and promptly from
// deep inside (its own bz_binder_begin_object / field-name-null check,
// already pending-checked) unwound straight past the two existing "dispose
// already-filled slots" loops (which only run on THIS class's own
// field-name-null / missing-required-field paths, not a nested recursive
// throw) — orphaning the already-read string. Fixed by pushing a dedicated
// disposeStack_ scope around the per-field loop that registers each
// owned-class/string component slot for disposal-on-unwind-only as soon as
// it is allocated, popped WITHOUT disposal once construction begins.
//
// FAILS WITHOUT / PASSES WITH: every runAotChecked test below whose comment
// names a leak size flips r.ran from false to true (confirmed via
// scratch-copy git-checkout revert of src/codegen/LlvmBackend_Serialization.cpp
// to its pre-#2378 state + rebuild + re-run, NOT git stash, then restored).

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
// Bug A: bz_binder_read_int's own exception object leaks when truncated
// input is detected reading a NESTED class's own scalar field.
//
// Reproduced verbatim from the issue: `Pair(owned first: Leaf, second:
// string)` — cutting the wire to HALF its length lands 4 bytes into Leaf's
// own 8-byte `v: int` value (byte offset 22 of the 53-byte wire; the byte
// offset in the caught exception's own message is asserted in a companion
// test below). Before the fix: 134 bytes / 3 allocations leaked (the
// bz_ser_throw'd exception object + its bz_make_string message's header and
// data buffer) — the exception that RAISED is a DIFFERENT, second one
// (emitThrowBuiltin's generic SerializationException, from the field-name
// read that fails next because the failed int read never advanced the wire
// position), orphaning the first.
// ===========================================================================
BEZEL_TEST(serialization_truncation_leak_2378,
           int_value_read_truncated_inside_nested_class_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(owned first: Leaf, second: string): ISerializable, IDeserializable "
        "  public owned First: Leaf := first; "
        "  public Second: string := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(new Leaf(9), \"tag-value\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var halfLen: int := bytes.Length / 2; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, halfLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 134-byte / 3-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Self-pass (b): the CAUGHT exception is the genuine one bz_ser_throw
// raised (not an empty/generic supersede-substitute) — its Message carries
// the original truncation site, not a swallowed/replaced one. Byte offset
// 22 matches the sweep's own computed region: Leaf's `v: int` value starts
// at wire offset 22 in this exact 53-byte layout.
BEZEL_TEST(serialization_truncation_leak_2378,
           int_value_truncation_exception_message_not_swallowed_spec_2_15) {
    auto r = runAot(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(owned first: Leaf, second: string): ISerializable, IDeserializable "
        "  public owned First: Leaf := first; "
        "  public Second: string := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(new Leaf(9), \"tag-value\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var halfLen: int := bytes.Length / 2; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, halfLen)); "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Truncated binary input reading int\") "
        "            then 42 else 1); "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Bug B: a successfully-read STRING field leaks when a LATER field's own
// read recurses into a nested class deserialize that throws.
//
// Reproduced verbatim from the issue's own follow-up comment: `Pair(first:
// string, owned second: Leaf)` — cutting the LAST 15 bytes off the wire
// lands inside Leaf's own 4-byte object-header length prefix (byte offset
// 57 of the 74-byte wire). `First`'s 30-character string value was already
// fully, successfully read and stored before Second's own nested read
// fails. Before the fix: 63 bytes / 2 allocations leaked (First's own
// bz_string header + its UTF-8 data buffer) even though First's read itself
// never truncated — it was simply never disposed once Second's read threw.
// ===========================================================================
BEZEL_TEST(serialization_truncation_leak_2378,
           string_field_orphaned_by_later_nested_class_truncation_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(first: string, owned second: Leaf): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public owned Second: Leaf := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(\"a-fairly-long-tag-value-string\", new Leaf(9)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 15; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 63-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Companion to the above: the truncation this time lands INSIDE Leaf's own
// field-name read ("V", not its header) — a second, distinct region within
// "a nested object's own read" that orphaned First's string identically
// before the fix (same 63-byte signature; verified in the byte-offset
// sweep). Cut leaves exactly 63 of the 74 bytes.
BEZEL_TEST(serialization_truncation_leak_2378,
           string_field_orphaned_by_nested_field_name_truncation_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(first: string, owned second: Leaf): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public owned Second: Leaf := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(\"a-fairly-long-tag-value-string\", new Leaf(9)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 11; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 63-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Bug A's worse-than-a-leak sibling: a truncated TRAILING scalar value read
// with nothing structural after it to trip an existing check used to
// silently "succeed" with garbage data (exit 0) instead of raising at all.
// Confirmed via the byte-offset sweep: before the fix this exact program
// returned 0; after, it correctly raises (42). No leak either way (nothing
// is allocated by a failed bz_binder_read_int call) — this test pins the
// CONTROL-FLOW correctness the fix restores as a side effect of no longer
// running the rest of the field loop on a stale pending exception.
// ===========================================================================
BEZEL_TEST(serialization_truncation_leak_2378,
           trailing_nested_int_value_truncation_no_longer_silently_succeeds_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(first: string, owned second: Leaf): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public owned Second: Leaf := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(\"a-fairly-long-tag-value-string\", new Leaf(9)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 4; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);  // 0 before the fix (silently "succeeded" with a garbage v)
}

// ===========================================================================
// "A truncation inside a NESTED object's own read" at two levels of
// nesting: Root owns Mid owns Leaf, cut lands precisely inside Leaf's own
// (deepest-level) int value read. Proves the disposal chain protects EVERY
// enclosing level's already-read fields (Root's own Id, Mid's own Name),
// not just the immediately-enclosing one.
// ===========================================================================
BEZEL_TEST(serialization_truncation_leak_2378,
           truncation_two_levels_deep_disposes_every_enclosing_field_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Mid(name: string, owned leaf: Leaf): ISerializable, IDeserializable "
        "  public Name: string := name; "
        "  public owned Leaf: Leaf := leaf; "
        "end Mid; "
        "class Root(id: int, owned mid: Mid): ISerializable, IDeserializable "
        "  public Id: int := id; "
        "  public owned Mid: Mid := mid; "
        "end Root; "
        "class C public static method Main(): int "
        "  using var r := new Root(7, new Mid(\"middle-tag\", new Leaf(9))); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        // Root's own Id (int) is read first, then Mid's own Name (string) is
        // read and stored successfully, THEN Mid's owned Leaf field recurses
        // into Leaf's own deserialize -- cut just short of full length lands
        // inside Leaf's own int value, orphaning BOTH Root.Id's slot (a
        // value type, nothing to leak) and Mid.Name's slot (a string --
        // something to leak) if the disposal chain doesn't reach through
        // two levels of recursion.
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Root.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Clean, full-read controls (no truncation): both wire shapes above, minus
// the cut. Round-trips correctly AND the fix's new per-slot disposal
// registration -- popped WITHOUT running its disposals once construction
// begins -- does not itself leak or double-free on the ordinary success
// path (this is what the #2378 fix's self-pass (c) also confirms via the
// existing 140-test `serialization` gtest filter; pinned again here
// directly against the exact two shapes this issue is about).
// ===========================================================================
BEZEL_TEST(serialization_truncation_leak_2378,
           clean_full_read_owned_class_first_shape_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(owned first: Leaf, second: string): ISerializable, IDeserializable "
        "  public owned First: Leaf := first; "
        "  public Second: string := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(new Leaf(9), \"tag-value\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var back := Pair.Deserialize(d); "
        "  if back.First.V <> 9 then return 1; end; "
        "  if back.Second <> \"tag-value\" then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(serialization_truncation_leak_2378,
           clean_full_read_string_first_shape_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(first: string, owned second: Leaf): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public owned Second: Leaf := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(\"a-fairly-long-tag-value-string\", new Leaf(9)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var back := Pair.Deserialize(d); "
        "  if back.First <> \"a-fairly-long-tag-value-string\" then return 1; end; "
        "  if back.Second.V <> 9 then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Adversarial self-pass (a): does fixing the leak introduce a DOUBLE-FREE on
// the path where an already-CONSUMED (fully-constructed, transferred) owned
// object -- itself owning a string field -- is followed by a later sibling
// field that throws? Pinned permanently, not just spot-checked, per the
// project's self-pass discipline.
// ===========================================================================
BEZEL_TEST(serialization_truncation_leak_2378,
           fully_constructed_nested_owned_string_field_not_double_disposed_spec_28_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf(tag: string, v: int): ISerializable, IDeserializable "
        "  public property Tag: string => tag; "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(owned first: Leaf, second: int): ISerializable, IDeserializable "
        "  public owned First: Leaf := first; "
        "  public Second: int := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(new Leaf(\"leaf-tag-value\", 9), 77); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        // First (a fully-constructed Leaf that itself owns a string field,
        // already read and transferred) is intact; the cut lands inside
        // Second's own int value.
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // a double-free/UAF here would flip r.ran false
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Adversarial self-pass (a), second angle: the PRE-EXISTING manual "dispose
// already-filled slots" loop (this class's OWN field-name-null truncation
// path) now self-nulls the slot it just disposed so this fix's new
// automatic per-slot sweep -- which ALSO covers the same slot, on the same
// unwind edge (emitThrowBuiltin's own emitUnwindDisposals call) -- does not
// double-dispose it. Two owned-class fields; the first is fully read and
// stored, the cut lands inside the second field's own NAME read (hits the
// manual loop directly, not a nested recursive throw).
BEZEL_TEST(serialization_truncation_leak_2378,
           errbb_manual_dispose_and_automatic_sweep_do_not_double_dispose_spec_28_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class TwoLeaves(owned a: Leaf, owned b: Leaf): ISerializable, IDeserializable "
        "  public owned A: Leaf := a; "
        "  public owned B: Leaf := b; "
        "end TwoLeaves; "
        "class C public static method Main(): int "
        "  using var p := new TwoLeaves(new Leaf(1), new Leaf(2)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := TwoLeaves.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // a double-free/UAF here would flip r.ran false
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
