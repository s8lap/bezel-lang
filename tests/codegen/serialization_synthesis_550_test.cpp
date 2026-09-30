// Codegen tests — §9.8 serialization synthesis (issue #550, tier 1).
//
// Tests BinarySerializer/BinaryDeserializer round-trips through the synthesized
// Serialize/Deserialize on records and enums, per §9.8 + stdlib §2.14–2.15.
//
// BinarySerializer/BinaryDeserializer live in System.Serialization (§2.15) and
// are NOT auto-imported; the tests include "import System.Serialization;".
//
// The JIT harness returns main's int exit code. We verify values by comparing
// deserialized output to expected using = (equality), returning 1 (pass) or 0
// (fail). All tests return 1 on success and 0 on failure.
//
// Scope: tier 1 only — records (primitives, string, enums, nullable T?,
// nested records), enums, `serialized` field clauses. Classes without the
// `: ISerializable`/`: IDeserializable` conformance are NOT synthesized; a call
// to Serialize/Deserialize on such a class without a hand-written method is a
// normal no-such-member error, not specially diagnosed here (type-driven, v0.141).
//
// The spec states (§2.14) that BinarySerializer/BinaryDeserializer require
// `import System.Serialization`. JsonSerializer/JsonDeserializer are deferred
// (not implemented in tier 1).
//
// Spec refs: §9.8, stdlib §2.14, §2.15.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool compileFails(const std::string& src) {
    auto r = runAot(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

// Preamble for all BinarySerializer tests (§2.15: requires import).
const char* kImport = "import System.Serialization; ";

} // namespace

// ===========================================================================
// §9.8 / §2.15: enum round-trip via BinarySerializer/BinaryDeserializer
// Enums serialize by member NAME (string), not underlying integer (§9.8:
// "The synthesized form writes the member *name* as a string via WriteString").
// ===========================================================================

// §9.8: basic enum round-trip — name written and read back correctly.
BEZEL_TEST(codegen_serialization_550, enum_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "enum Color: ISerializable, IDeserializable Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  Color.Green.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Color.Deserialize(d); "
        "  return (if v = Color.Green then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: enum round-trip — different constant (last).
BEZEL_TEST(codegen_serialization_550, enum_roundtrip_last_member_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "enum Dir: ISerializable, IDeserializable North, South, East, West; end Dir; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  Dir.West.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Dir.Deserialize(d); "
        "  return (if v = Dir.West then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: enum first constant round-trip.
BEZEL_TEST(codegen_serialization_550, enum_roundtrip_first_member_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "enum Priority: ISerializable, IDeserializable Low, Medium, High; end Priority; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  Priority.Low.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Priority.Deserialize(d); "
        "  return (if v = Priority.Low then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: enum serialized "name" clause — constant encoded under the alternative
// name, not the declared identifier.
BEZEL_TEST(codegen_serialization_550, enum_serialized_name_clause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "enum State: ISerializable, IDeserializable "
        "  On serialized \"on\", Off serialized \"off\"; "
        "end State; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  State.On.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := State.Deserialize(d); "
        "  return (if v = State.On then 1 else 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §9.8 / §2.15: positional record round-trips with BinarySerializer
// ===========================================================================

// §9.8 example 1: a positional record with int fields round-trips.
// This is the canonical §9.8 spec example: Point(X: int, Y: int).
BEZEL_TEST(codegen_serialization_550, record_int_fields_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 7); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var p2 := Point.Deserialize(d); "
        "  return (if p2.X = 3 and p2.Y = 7 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: a positional record with a string field round-trips.
BEZEL_TEST(codegen_serialization_550, record_string_field_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Named(Name: string, Age: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var n := new Named(\"Alice\", 30); "
        "  using var s := new BinarySerializer(); "
        "  n.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var n2 := Named.Deserialize(d); "
        "  return (if n2.Name = \"Alice\" and n2.Age = 30 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: a positional record with bool, int, double fields round-trips.
BEZEL_TEST(codegen_serialization_550, record_bool_double_fields_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Flags(Active: bool, Value: double): "
        "  ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var f := new Flags(true, 3.14); "
        "  using var s := new BinarySerializer(); "
        "  f.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var f2 := Flags.Deserialize(d); "
        "  return (if f2.Active and f2.Value = 3.14 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: a positional record with an enum field round-trips.
BEZEL_TEST(codegen_serialization_550, record_enum_field_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "enum Status: ISerializable, IDeserializable Active, Inactive; end Status; "
        "record Item(Name: string, St: Status): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var it := new Item(\"thing\", Status.Active); "
        "  using var s := new BinarySerializer(); "
        "  it.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var it2 := Item.Deserialize(d); "
        "  return (if it2.Name = \"thing\" and it2.St = Status.Active then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: nested record (tree shape) round-trips.
BEZEL_TEST(codegen_serialization_550, record_nested_record_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "record Line(A: Point, B: Point): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var ln := new Line(new Point(1, 2), new Point(3, 4)); "
        "  using var s := new BinarySerializer(); "
        "  ln.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var ln2 := Line.Deserialize(d); "
        "  return (if ln2.A.X = 1 and ln2.A.Y = 2 "
        "             and ln2.B.X = 3 and ln2.B.Y = 4 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: `serialized "name"` clause — field encoded under the alternative name.
// Per §9.8: field-name customization works in both directions.
BEZEL_TEST(codegen_serialization_550, record_serialized_name_clause_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record User(Id: int, "
        "  FirstName: string serialized \"first_name\"): "
        "  ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var u := new User(42, \"Bob\"); "
        "  using var s := new BinarySerializer(); "
        "  u.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var u2 := User.Deserialize(d); "
        "  return (if u2.Id = 42 and u2.FirstName = \"Bob\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: `serialized skip` — skipped field not written/read; deserialized
// from constructor with default value.
// §9.8: `serialized skip` — skipped field not written/read; deserialized
// from constructor with default value (default(int) = 0).
BEZEL_TEST(codegen_serialization_550, record_serialized_skip_clause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C2 end C2; "
        "record R(X: int, Secret: int serialized skip): "
        "  ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(5, 99); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.X = 5 and r2.Secret = 0 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: `serialized skip` on a BODY record (auto-property record, not
// positional) — fix #1612. emitRecordSerialize's BeginObject field count for
// a body record was computed as the FULL storage-symbol count (skip-marked
// auto-properties included) rather than filtering skip components the way
// the positional branch already did, so the wire header declared one more
// field than the write loop actually wrote. Deserialize's read loop, driven
// by the (over-)declared wire count, then ran an extra iteration past the
// real field data — a null field-name read dereferenced before the fix
// (SIGSEGV); after the fix the count matches the fields actually written and
// the round-trip is lossless for the retained (non-skip) field.
BEZEL_TEST(codegen_serialization_550, body_record_serialized_skip_clause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Cached: ISerializable, IDeserializable "
        "  public readonly property Id: int; "
        "  public readonly property Cache: int serialized skip; "
        "  public constructor(id: int, cache: int) "
        "    Id := id; Cache := cache; end constructor; "
        "end Cached; "
        "class C public static method Main(): int "
        "  var original := new Cached(7, 1); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Cached.Deserialize(d); "
        // Cache was excluded; Deserialize supplies default(int) = 0.
        "  return (if back.Id = 7 and back.Cache = 0 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8 / §2.15 field order independence: Deserialize must work regardless of
// input field order (dispatch by name). We test by serializing normally and
// reading back — the binary format writes declaration order, reads by name.
BEZEL_TEST(codegen_serialization_550, record_deserialize_by_field_name_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(A: int, B: int, C: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(10, 20, 30); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.A = 10 and r2.B = 20 and r2.C = 30 then 1 else 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §9.8: a record NOT declaring ISerializable has no synthesized Serialize →
// calling it is a normal no-member error (compile fails).
// ===========================================================================

// §9.8: calling Serialize on a record that does not declare ISerializable
// is a compile error (no synthesis without the conformance declaration).
BEZEL_TEST(codegen_serialization_550, no_conformance_no_synthesis_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "record R(X: int); "
        "class C public static method Main(): int "
        "  var r := new R(1); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "   // no ISerializable declared → compile error
        "  return 0; "
        "end Main; end C;"));
}

// ===========================================================================
// §9.8 / §2.15: classes declaring `: ISerializable` use type-driven synthesis
// (v0.141, keyword clause retired). No hand-written method required.
// ===========================================================================

// §9.8 v0.141 (#703 migration): the keyword clause was retired — a class that
// declares `: ISerializable` (serialize-only, no IDeserializable) without a
// hand-written Serialize is now type-driven-synthesized, not a compile error.
// A keyword-less serialize-only Box compiles and serializes its value field.
BEZEL_TEST(codegen_serialization_550,
           class_iserializable_only_keyword_less_serializes_spec_9_8_v0141) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Box: ISerializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(1); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// §9.8 v0.182 (#1572): record Deserialize CONSTRUCTOR SELECTION — the
// synthesized Deserialize CALLS a record's unique explicit constructor with
// the deserialized values as arguments (rather than the pre-v0.182 behavior
// of always writing components directly at the storage level, bypassing any
// explicit constructor entirely).
// ===========================================================================

// §9.8 v0.182: a body record with an explicit NORMALIZING constructor
// (doubles its argument) — Deserialize re-runs the constructor against the
// WIRE value, not against the pre-normalized original. original.N := 5*2=10
// is what gets serialized; deserializing calls the constructor again with
// n=10, which doubles again to 20. This directly distinguishes the v0.182
// "call the constructor" model from the retired storage-level-always model
// (which would have produced 10, an unchanged round-trip).
BEZEL_TEST(codegen_serialization_550,
           record_explicit_ctor_rerun_on_deserialize_spec_9_8_v0182) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Doubled: ISerializable, IDeserializable "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n * 2; end constructor; "
        "end Doubled; "
        "class C public static method Main(): int "
        "  var original := new Doubled(5); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Doubled.Deserialize(d); "
        "  return back.N; "
        "end Main; end C;"), 20);
}

// §9.8 v0.182: a COMBINED-form record (positional fields + an explicit body
// constructor) round-trips through the same constructor-call model — the
// positional fields count as storage components alongside any body
// auto-properties (§9.3), and the sole explicit constructor is called with
// the deserialized X value, which it offsets by +100. original.X = 1+100=101
// is what's on the wire; deserializing re-runs the constructor with x=101,
// giving 101+100=201 (NOT 101, which the retired storage-level-always model
// would have produced).
BEZEL_TEST(codegen_serialization_550,
           record_combined_form_explicit_ctor_rerun_spec_9_8_v0182) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Vec2(X: int): ISerializable, IDeserializable "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) "
        "    X := x + 100; "
        "    Y := y + 100; "
        "  end constructor; "
        "end Vec2; "
        "class C public static method Main(): int "
        "  var original := new Vec2(1, 2); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Vec2.Deserialize(d); "
        "  return back.X; "
        "end Main; end C;"), 201);
}

// §9.8 v0.182: "A record that declares no explicit constructor is
// constructed directly from the read components at the storage level" — a
// purely positional record (no body, no explicit constructor) still
// round-trips its exact field values unchanged (storage-level construction,
// UNAFFECTED by the constructor-selection change above).
BEZEL_TEST(codegen_serialization_550,
           record_positional_no_ctor_storage_level_roundtrip_spec_9_8_v0182) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var original := new Point(3, 4); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Point.Deserialize(d); "
        "  return back.X * 10 + back.Y; "
        "end Main; end C;"), 34);
}

// §9.8 v0.182: a body record with NO explicit constructor also uses
// storage-level construction (unaffected) — a plain auto-equality-style
// record still round-trips its exact stored values.
BEZEL_TEST(codegen_serialization_550,
           record_body_no_ctor_storage_level_roundtrip_spec_9_8_v0182) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Plain: ISerializable, IDeserializable "
        "  public property N: int; "
        "end Plain; "
        "class C public static method Main(): int "
        "  var original := new Plain() with(N := 7); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Plain.Deserialize(d); "
        "  return back.N; "
        "end Main; end C;"), 7);
}
