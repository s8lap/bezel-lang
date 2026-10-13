// Spec-derived codegen tests — §9.8 (v0.228) class serialization synthesis
// for PRIMARY-CONSTRUCTOR classes. Issue #2325.
//
// serialization_tier2_classes_test.cpp already covers class serialization
// extensively — but every one of its classes uses an EXPLICIT
// `constructor(...)` body plus `property X: T;` auto-properties. That shape
// never touches a class's PRIMARY-constructor parameter list at all, so it
// never exercised the two representation bugs #2325 diagnoses:
//
//   1. A primary-ctor-parameter-derived FieldSymbol (posParam != null) has
//      `decl` set to the owning TypeDecl rather than a FieldDecl/PropertyDecl
//      (Symbol.hpp), so the component-collection code in both
//      checkSerializationEligibility (sema) and collectClassComponents
//      (codegen) silently skipped it — a class exposing a primary-ctor
//      parameter ONLY through a computed property (`property Id: int =>
//      id;`, exactly stdlib §2.14's own Order/Invoice worked examples, and
//      the issue's own repro) reported an EMPTY component set.
//   2. A class's implicit PRIMARY constructor has no ConstructorSymbol at all
//      (SymbolCollection.cpp creates one only for an explicit
//      ast::ConstructorDecl), so the §9.8 constructor-selection check never
//      considered it a match candidate — "candidates: (none)" even when the
//      primary constructor's own parameter list is the obvious match.
//
// This file is exactly the primary-constructor idiom the stdlib worked
// examples use (Bezel_Standard_Library.md §2.14's Order/Invoice), covering
// the task's slices (2) plain-scalar fields, (3) string field ownership, and
// (4) nested serialization-eligible class/record fields — plus adversarial
// rejection cases (a class that must still be rejected is not newly admitted)
// per CLAUDE.md's self-pass discipline.
//
// Spec: §9.8 (v0.228) "Class serialization synthesis" — "it needs no header
// shape clause, because the serialized shape is read from the field types";
// "The serialized components of a class are its `owned` and value-typed
// fields and auto-properties ... in declaration order"; §11.1 "Primary
// constructors" — "parameters referenced only in initializers do not
// generate backing fields; parameters referenced in method, property,
// constructor, or destructor bodies are captured into compiler-generated
// private fields."

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// runAotChecked returns ran=false on any sanitizer fire (SEGV / leak / UAF);
// map that to a sentinel so a single EXPECT_EQ pins BOTH memory-cleanliness
// AND the round-tripped value (mirrors async_array_frame_spill_2132_test.cpp).
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

bool compileFails(const std::string& src) {
    auto r = runAot(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

const char* kImport = "import System.Serialization; ";

}  // namespace

// ===========================================================================
// Slice 2 — plain-scalar-field primary-constructor classes.
//
// §9.8: "A class opts into serialization synthesis by declaring
// `: ISerializable` and/or `: IDeserializable`; it needs no header shape
// clause." §11.1: a primary-ctor parameter referenced ONLY inside a
// computed-property body (`property Id: int => id;`) is captured into a
// compiler-generated private field — a genuine value-typed field per §9.8's
// component rule.
// ===========================================================================

// The issue's own repro, verbatim: a class exposing BOTH primary-ctor
// parameters ONLY through computed properties (`=> id` / `=> name`), no
// explicit fields at all. Before the fix this reported component set {} and
// "candidates: (none)" even though the class is exactly stdlib §2.14's
// `Order.Id` idiom applied to every field.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           computed_property_only_capture_roundtrip_spec_9_8_sec11_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Widget(id: int, name: string): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public property Name: string => name; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(7, \"gadget\"); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget.Deserialize(d); "
        "  if w2.Id <> 7 then return 91; end; "
        "  return (if w2.Name = \"gadget\" then 42 else 92); "
        "end Main; end C;"), 42);
}

// A primary-ctor class whose scalar fields are EXPLICIT (`Id: int := id;`),
// not computed properties — exercises the OTHER half of the fix: the
// constructor-selection check must recognize the class's own primary
// constructor as a match candidate (there is no ConstructorSymbol for it).
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           explicit_field_primary_ctor_roundtrip_spec_9_8_sec11_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Widget2(id: int, name: string): ISerializable, IDeserializable "
        "  public Id: int := id; "
        "  public Name: string := name; "
        "end Widget2; "
        "class C public static method Main(): int "
        "  using var w := new Widget2(11, \"box\"); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget2.Deserialize(d); "
        "  if w2.Id <> 11 then return 91; end; "
        "  return (if w2.Name = \"box\" then 42 else 92); "
        "end Main; end C;"), 42);
}

// A class mixing BOTH storage shapes in one primary constructor: one param
// captured only via a computed property, the other consumed by an explicit
// field initializer — exactly the stdlib §2.14 `Order` shape (`Id` computed,
// `Customer`/`Lines` explicit `owned` fields), scalar-only version. Exercises
// both branches of collectClassComponents / checkComponentType together.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           mixed_computed_and_explicit_field_roundtrip_spec_2_14_order_shape) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Item(id: int, label: string): ISerializable, IDeserializable "
        "  public Label: string := label; "        // explicit field
        "  public property Id: int => id; "          // computed-property capture
        "end Item; "
        "class C public static method Main(): int "
        "  using var it := new Item(3, \"widget\"); "
        "  using var s := new BinarySerializer(); "
        "  it.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var it2 := Item.Deserialize(d); "
        "  if it2.Id <> 3 then return 91; end; "
        "  return (if it2.Label = \"widget\" then 42 else 92); "
        "end Main; end C;"), 42);
}

// The empty/default case: a primary-ctor class round-trips a zero/empty
// scalar value correctly (not just a "truthy" nonzero/nonempty one).
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           default_zero_and_empty_string_values_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Widget(id: int, name: string): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public property Name: string => name; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(0, \"\"); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget.Deserialize(d); "
        "  if w2.Id <> 0 then return 91; end; "
        "  return (if w2.Name = \"\" then 42 else 92); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Slice 3 — string-field ownership: disposal-exactly-once under ASan/LSan.
//
// A synthesized Deserialize that leaks or double-frees a captured string is
// worse than one that does not exist (per the run brief / #2326's hazard).
// The deserialized string is a FRESH +1 read off the wire (bz_binder_read_
// string); the fix stores it directly into the primary-ctor parameter's own
// struct slot (transfer, no retain — LlvmBackend_Exprs2.cpp's #1605 comment
// establishes this is correct for a fresh +1 producer), then lets
// emitInstanceInit's ordinary field-initializer codegen do whatever it
// already does for `new Widget(...)` construction (retain-on-copy into an
// explicit field). Both storage shapes (computed-property-only capture, and
// explicit-field consumption) are exercised.
// ===========================================================================

BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           computed_property_string_capture_no_leak_no_double_free_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Widget(id: int, name: string): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public property Name: string => name; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(7, \"gadget-name\"); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget.Deserialize(d); "
        "  return (if w2.Name = \"gadget-name\" then 42 else 91); "
        "end Main; end C;"), 42);
}

// The explicit-field string variant (Widget2 shape) under the same ASan/LSan
// check — the field-initializer RETAIN path (`Name: string := name;`) is the
// one most at risk of a leak (two independent storage slots — the captured
// posParam slot and the explicit field slot — each holding their own
// reference) or a double-free (releasing the same pointer through both).
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           explicit_field_string_no_leak_no_double_free_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Widget2(id: int, name: string): ISerializable, IDeserializable "
        "  public Id: int := id; "
        "  public Name: string := name; "
        "end Widget2; "
        "class C public static method Main(): int "
        "  using var w := new Widget2(11, \"boxed-value\"); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget2.Deserialize(d); "
        "  return (if w2.Name = \"boxed-value\" then 42 else 91); "
        "end Main; end C;"), 42);
}

// Multiple round-trips in a loop (repeated allocate/dispose cycles) — a
// leak-per-iteration bug would show up as a large, clearly-nonzero leak
// report rather than a single missed free that a one-shot test might mask
// under LSan's allocator noise floor.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           repeated_roundtrip_no_accumulating_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Widget(id: int, name: string): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public property Name: string => name; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var total: int := 0; "
        "  var i: int := 0; "
        "  while i < 50 do "
        "    using var w := new Widget(i, \"item\"); "
        "    using var s := new BinarySerializer(); "
        "    w.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    using var w2 := Widget.Deserialize(d); "
        "    total := total + w2.Id; "
        "    i := i + 1; "
        "  end; "
        // sum 0..49 = 1225; 1225 % 256 = 201 (exit codes are 8-bit) — use a
        // reduction that fits cleanly instead.
        "  return (if total = 1225 then 42 else total); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Slice 4 — nested serialization-eligible class/record fields on a
// primary-constructor class.
//
// §9.8: "A class is serialization-eligible when every serialized component
// is itself serialization-eligible (recursively admitting other conforming
// classes ...)." §2.14's own worked example: `public class Order(id: int,
// owned customer: Customer, ...): ISerializable, IDeserializable ... public
// owned Customer: Customer := customer; ... public property Id: int => id;`.
// ===========================================================================

// The stdlib §2.14 Order/Customer shape, scalar-simplified: an OWNED nested
// class field consumed by an explicit field initializer, alongside a
// computed-property-captured scalar id — both PRIMARY-constructor classes
// (Customer itself uses no explicit constructor either), so the nested
// Deserialize call also exercises the primary-ctor construction path
// recursively.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           owned_nested_primary_ctor_class_field_roundtrip_spec_2_14_order) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Customer(name: string): ISerializable, IDeserializable "
        "  public property Name: string => name; "
        "end Customer; "
        "class Order(id: int, owned customer: Customer): ISerializable, IDeserializable "
        "  public owned Customer: Customer := customer; "
        "  public property Id: int => id; "
        "end Order; "
        "class C public static method Main(): int "
        "  using var o := new Order(42, new Customer(\"Ada\")); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var o2 := Order.Deserialize(d); "
        "  if o2.Id <> 42 then return 91; end; "
        "  return (if o2.Customer.Name = \"Ada\" then 42 else 92); "
        "end Main; end C;"), 42);
}

// Disposal-exactly-once for the owned-nested-class shape: the deserialized
// Customer is constructed once (via the recursive primary-ctor path), stored
// once into Order's explicit `Customer` field, and disposed exactly once when
// `o2` goes out of scope. A double-construction or a missed transfer would
// show up as a leak or a double-free under ASan/LSan.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           owned_nested_class_disposal_exactly_once_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Customer(name: string): ISerializable, IDeserializable "
        "  public property Name: string => name; "
        "end Customer; "
        "class Order(id: int, owned customer: Customer): ISerializable, IDeserializable "
        "  public owned Customer: Customer := customer; "
        "  public property Id: int => id; "
        "end Order; "
        "class C public static method Main(): int "
        "  using var o := new Order(42, new Customer(\"Ada\")); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var o2 := Order.Deserialize(d); "
        "  return (if o2.Customer.Name = \"Ada\" then 42 else 91); "
        "end Main; end C;"), 42);
}

// A primary-ctor class containing a serialization-eligible RECORD field
// (record-admissible, §9.8's record clause) — the class-side mirror of
// serialization_tier2_classes_test.cpp's tree_class_containing_record_field
// test, but with the outer class itself using a primary constructor and a
// computed-property-only-captured Center field.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           primary_ctor_class_containing_record_field_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "class Shape(center: Point, label: string): ISerializable, IDeserializable "
        "  public property Center: Point => center; "
        "  public Label: string := label; "
        "end Shape; "
        "class C public static method Main(): int "
        "  using var sh := new Shape(new Point(3, 4), \"circle\"); "
        "  using var s := new BinarySerializer(); "
        "  sh.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var sh2 := Shape.Deserialize(d); "
        "  if sh2.Center.X <> 3 then return 91; end; "
        "  if sh2.Center.Y <> 4 then return 92; end; "
        "  return (if sh2.Label = \"circle\" then 42 else 93); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Adversarial self-pass — a class that MUST still be rejected must not be
// newly admitted by removing/bypassing the old header-clause-shaped checks.
// ===========================================================================

// A primary-ctor class whose computed-property-captured field has a type
// that is NOT serialization-eligible (an interface — never eligible per
// §9.8) must still be a compile error at the conformance declaration, exactly
// as an explicit-constructor class with the same defect already is
// (serialization_tier2_classes_test.cpp's tree_class_ineligible_owned_child_
// is_error_spec_9_8 covers the explicit-ctor shape; this is the primary-ctor
// mirror).
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           computed_property_capture_of_ineligible_type_is_error_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "interface IThing public method Go(): int; end IThing; "
        "class Widget(thing: IThing): ISerializable, IDeserializable "
        // `thing` is captured via a body reference (Describe reads it), so it
        // IS a component per §11.1 — and IThing is never serialization-
        // eligible (§9.8's component-type list has no interface entry).
        "  public method Describe(): int return thing.Go(); end Describe; "
        "end Widget; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// A primary-ctor class whose ctor parameter names do NOT match its component
// set (mirrors serialization_tier2_classes_test.cpp's
// ctor_param_name_no_match_is_compile_error, but for the IMPLICIT primary
// constructor rather than an explicit one): the primary ctor's own parameter
// is captured under one name, but an explicit field with a DIFFERENT
// (unrelated) name is also declared, so no single constructor (primary or
// otherwise) binds the full component set.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           primary_ctor_param_name_mismatch_is_compile_error_spec_9_8_v0136) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        // Widget's primary ctor is (id: int) — one param. But TWO components
        // exist: `id` itself (captured by the computed property) AND the
        // explicit field `Extra` (initialized from a literal, not from a
        // ctor param) — so the 1-param primary ctor can never bind both.
        "class Widget(id: int): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public Extra: int := 99; "
        "end Widget; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// A record still cannot hold a class-typed component even when the class
// uses a primary constructor (§9.8's record clause is unaffected by #2325 —
// this pins that the fix did not accidentally loosen record admissibility).
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           record_with_primary_ctor_class_field_still_rejected_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Bare(x: int): ISerializable, IDeserializable "
        "  public property X: int => x; "
        "end Bare; "
        "record R(Id: int, Obj: Bare): ISerializable;"));
}

// The retired `serialized tree`/`graph`/`cyclic` header-clause KEYWORDS are
// not reintroduced or required by the fix — a primary-ctor class declares
// eligibility with ONLY `: ISerializable, IDeserializable`, no header clause
// of any kind, and still synthesizes (positive control restating the point
// already demonstrated by every round-trip test above; kept as an explicit,
// narrowly-named pin so a future regression toward the old model is caught
// immediately by name).
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           no_header_clause_required_spec_9_8_v0228) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Widget(id: int): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(5); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget.Deserialize(d); "
        "  return (if w2.Id = 5 then 42 else 91); "
        "end Main; end C;"), 42);
}

// Adversarial self-pass (b): a mid-deserialize failure (malformed/truncated
// wire input) on the NEW primary-ctor path must raise SerializationException
// cleanly, with no leak of the partially-reconstructed owned nested Customer
// — mirrors serialization_tier2_classes_test.cpp's
// truncated_input_asan_no_leak_spec_9_8_v0136, but for a primary-ctor Order/
// Customer pair. The truncation is detected by the WIRE-READING loop, which
// runs entirely BEFORE the primary-ctor construction path this fix adds (the
// class instance is not even calloc'd yet), so this exercises the
// pre-existing missing-field / truncated-stream cleanup with the new
// component-collection code providing the component list — a leak here would
// mean the fix's wider component set (now including captured primary-ctor
// params) confused that pre-existing cleanup accounting.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           truncated_input_no_leak_primary_ctor_nested_owned_spec_9_8) {
    const std::string kProg =
        std::string("module M; ") + kImport +
        "class Customer(name: string): ISerializable, IDeserializable "
        "  public property Name: string => name; "
        "end Customer; "
        "class Order(id: int, owned customer: Customer): ISerializable, IDeserializable "
        "  public owned Customer: Customer := customer; "
        "  public property Id: int => id; "
        "end Order; "
        "class C public static method Main(): int "
        "  using var o := new Order(42, new Customer(\"Ada\")); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var halfLen: int := bytes.Length / 2; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, halfLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Order.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; "
        "  end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;";
    // Exit-code leg: SerializationException is caught.
    BEZEL_EXPECT_EQ(run(kProg), 42);
    // ASan/LSan leg: no leak of the partial Order/Customer reconstruction.
    BEZEL_EXPECT_EQ(runChecked(kProg), 42);
}

// ===========================================================================
// Bonus verification (informs #718's Shared<T> spine work, not this fix's
// scope): a primary-ctor class with an `owned Shared<T>` field (§9.8's
// identity-participant / graph-mode component) round-trips cleanly. §9.8's
// PRE-PASS (checkSerializationEligibility, unaffected by this fix) already
// classifies a class with a Shared<T>/Weak<T> field as graph-mode from field
// TYPES alone; this pins that the primary-ctor construction path this fix
// adds composes correctly with that pre-existing graph-mode machinery
// (identity-tag table, bz_shared_new/Clone) — including disposal, since a
// naive probe of this shape WITHOUT the `owned` keyword on both the
// primary-ctor parameter and the explicit field leaks (stdlib §2.16:
// "Handles are ordinary owned class instances" — the same `owned` discipline
// as any other owned field, unrelated to serialization).
// ===========================================================================

BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           owned_shared_field_primary_ctor_roundtrip_spec_9_8_sec2_16) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Hub(owned target: Shared<Leaf>): ISerializable, IDeserializable "
        "  public owned Target: Shared<Leaf> := target; "
        "end Hub; "
        "class C public static method Main(): int "
        "  using var leaf := new Shared<Leaf>(new Leaf(9)); "
        "  using var hub := new Hub(leaf.Clone()); "
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        "  return (if hub2.Target.Get().V = 9 then 42 else 91); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Coordinator-directed follow-up self-pass (does recognizing the primary
// constructor as a §9.8 constructor-selection candidate newly ADMIT any
// class the spec says is INELIGIBLE?). Three findings from that review:
//
//   1. A primary-ctor class with a base class that carries its OWN storage,
//      declaring IDeserializable, is a REAL, REACHABLE silent-corruption bug
//      this fix's recognition of the primary constructor opens: the
//      synthesized Deserialize (both this fix's primary-ctor path AND the
//      pre-existing explicit-ctor path — the underlying defect is shared,
//      neither ever constructs a base subobject) round-trips the base's
//      field as its calloc-zeroed default instead of the deserialized
//      value, with NO diagnostic, prior to the fix below. Confirmed by
//      directly printing the round-tripped base field (not merely asserting
//      a wrong exit code): `Derived().V` came back 0 instead of the
//      serialized 999. Closed by rejecting synthesis of Deserialize for a
//      class whose base chain carries storage (issue #2364) — this pins the
//      rejection, mechanically distinguishing it from the ALREADY-PASSING
//      §9.8 tier-4 polymorphic pattern (an abstract base with ZERO storage,
//      serialization_tier2_classes_test.cpp's polymorphic_dispatch_*
//      tests), which remains admitted because there is no base storage to
//      lose.
//   2. A borrowing (non-owned) class-typed field captured ONLY via a body
//      reference (no explicit field) is STILL correctly rejected under
//      `serialized tree` mode — the back-edge-forbidden-under-tree check
//      (checkComponentType, shared verbatim between the FieldDecl and
//      posParam branches) was not weakened by this fix.
//   3. An OWNED class-typed field captured ONLY via a computed property (no
//      explicit field) — a case this run's hand-off had flagged as
//      "untested, narrow risk" — round-trips AND disposes cleanly. Verified
//      empirically rather than left as a guess.
// ===========================================================================

// Finding 1 (negative): primary-ctor class inheriting from a base with its
// own storage, declaring IDeserializable, is a compile error naming the
// base and issue #2364 — not a silent corruption.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           primary_ctor_class_inheriting_base_with_storage_is_error_spec_9_8_2364) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Base(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Base; "
        "class Derived(): Base(999), ISerializable, IDeserializable "
        "  public property Tag: int => 42; "
        "end Derived; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// Finding 1 (positive control): the identical inheritance shape but
// ISerializable ONLY (no IDeserializable) is NOT rejected — writing an
// already-constructed instance's inherited fields has no construction step,
// so there is nothing unsound to reject. Also confirms Serialize actually
// walks the base chain correctly (a non-empty buffer proves the base field
// was written, not merely that compilation succeeded).
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           primary_ctor_class_inheriting_base_serialize_only_still_works_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Base(v: int): ISerializable "
        "  public property V: int => v; "
        "end Base; "
        "class Derived(): Base(999), ISerializable "
        "  public property Tag: int => 42; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d1 := new Derived(); "
        "  using var s := new BinarySerializer(); "
        "  d1.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  return (if bytes.Length > 0 then 42 else 91); "
        "end Main; end C;"), 42);
}

// Finding 1 (positive control): the §9.8 tier-4 polymorphic pattern — a base
// with NO storage of its own (only a `polymorphic` registry member and an
// abstract method) — is NOT newly rejected by the base-storage check, even
// though `Circle`/`Square` do inherit from a class (`Shape`) and declare
// IDeserializable. Mirrors serialization_tier2_classes_test.cpp's
// polymorphic_dispatch_circle_roundtrip_spec_9_8 with a scalar-valued
// (rather than owned-collection-valued) Container field, kept minimal here
// since that file already covers the full round-trip.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           polymorphic_zero_storage_base_not_rejected_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "abstract class Shape : ISerializable, IDeserializable "
        "  polymorphic(\"circle\": Circle); "
        "  public abstract method Kind(): int; "
        "end Shape; "
        "class Circle(radius: double) : Shape, ISerializable, IDeserializable "
        "  public property Radius: double => radius; "
        "  public override method Kind(): int return 1; end Kind; "
        "end Circle; "
        "class Container(owned s: Shape): ISerializable, IDeserializable "
        "  public owned S: Shape := s; "
        "end Container; "
        "class C public static method Main(): int "
        "  using var box := new Container(new Circle(5.0)); "
        "  using var s := new BinarySerializer(); "
        "  box.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var box2 := Container.Deserialize(d); "
        "  return (if box2.S.Kind() = 1 then 42 else 91); "
        "end Main; end C;"), 42);
}

// Finding 2: a borrowing class-typed field captured ONLY via a body
// reference (no explicit field) is still correctly rejected under
// `serialized tree` (the default shape absent a Shared<T>/Weak<T>/back-edge
// field) — the back-edge-forbidden check was not weakened by recognizing
// captured posParam fields as components.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           borrowing_backedge_captured_only_by_body_still_rejected_under_tree_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Hub(leaf: Leaf): ISerializable, IDeserializable "
        "  public method Peek(): int return leaf.V; end Peek; "
        "end Hub; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// Finding 3: an OWNED class-typed field captured ONLY via a computed
// property (no explicit field consuming it) round-trips AND disposes
// cleanly — verified rather than left as an untested risk.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           owned_class_captured_only_by_property_roundtrip_no_leak_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Hub(owned leaf: Leaf): ISerializable, IDeserializable "
        "  public property Peek: int => leaf.V; "
        "end Hub; "
        "class C public static method Main(): int "
        "  using var hub := new Hub(new Leaf(9)); "
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        "  return (if hub2.Peek = 9 then 42 else 91); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Issue #2362 — primary-ctor + explicit-ctor Deserialize combination
// (§9.8, v0.236).
//
// #2325 (above) verified the primary-ctor-ONLY Deserialize path; the
// explicit-ctor path (serialization_tier2_classes_test.cpp) was already long
// established. #2362 is the VERIFICATION GAP between them: a class with BOTH
// a primary constructor (§11.1) AND an explicit `constructor(...)` that
// chains to it via `this(...)`, where §9.8's constructor-selection check
// (line 1438: "When the class declares more than one constructor, the
// synthesized Deserialize uses the *unique* constructor whose parameter list
// binds exactly the owning-tree components and plain borrowing fields, matched
// by name") picks the EXPLICIT ctor — not the primary — as the unique
// bijection match.
//
// The shape: the primary ctor captures SOME components (here `id`, exposed
// via a computed property), but an auto-property component (`Extra`) exists
// that the primary ctor's parameter list does NOT cover, so the primary ctor
// alone cannot bijection-match (1 param vs 2 components). The explicit
// `constructor(id, extra)` is the unique match. The question #2362 asks:
// does Widget.Deserialize(d) correctly populate BOTH `Id` (via the `this(id)`
// chain running inside the explicit ctor — which captures the primary-ctor
// param) AND `Extra` (via the explicit ctor's own `Extra := extra` body
// assignment)?
//
// Verified CORRECT on origin/main (3a668dd): emitClassDeserialize's
// matchedCtor branch (LlvmBackend_Serialization.cpp) calls the explicit
// ctor's compiled LLVM body, whose emitConstructorBody runs the `this(id)`
// chain call (emitChainCall, isThis=true — delegates to the primary ctor's
// param-capture + field initializers per §22.9 step 0/2, with field
// initializers running exactly once at the end of the this-chain per the
// "Field initializers run exactly once" rule, spec line 4888), THEN the
// explicit ctor's own body (`Extra := extra`). Both fields round-trip; no
// code change warranted. These tests pin that combination durably.
//
// Note on the both-match ambiguity (adversarial self-pass): a class whose
// primary ctor AND an explicit ctor both bijection-match the component set
// is STRUCTURALLY IMPOSSIBLE — §11.1 line 1713 forbids redeclaring a
// constructor with the same parameter list as the primary constructor, so
// no ambiguity can arise between the primary and a chaining explicit ctor.
// The §9.8 "more than one matches" compile error is reachable only between
// two EXPLICIT ctors, which is already covered by
// serialization_tier2_classes_test.cpp.
// ===========================================================================

// The issue's own repro, verbatim shape: a class exposing the primary-ctor
// param `id` ONLY through a computed property (`=> id`), PLUS an auto-
// property `Extra` populated solely by the explicit ctor's body. Component
// set {id, Extra}; the 1-param primary ctor cannot bijection-match, so the
// 2-param explicit ctor is the unique match. Asserts BOTH Id AND Extra
// round-trip, under ASan/LSan (the string Extra is the field at risk of a
// leak/double-free through the this-chain + body-assignment path).
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           primary_plus_explicit_ctor_chain_roundtrip_spec_9_8_2362) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Widget(id: int): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public Extra: string := \"\"; "
        "  public constructor(id: int, extra: string) "
        "    this(id); "
        "    Extra := extra; "
        "  end constructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(7, \"gadget\"); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget.Deserialize(d); "
        "  if w2.Id <> 7 then return 91; end; "
        "  return (if w2.Extra = \"gadget\" then 42 else 92); "
        "end Main; end C;"), 42);
}

// A scalar-only variant (Extra is `int`, not `string`) — removes string
// ownership from the picture and isolates the construction-order question:
// does the explicit ctor's body assignment (`Extra := extra`) correctly win
// over the auto-property's own default initializer (`Extra: int := 0`), and
// does the `this(id)` chain correctly populate the primary-ctor-captured Id?
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           primary_plus_explicit_ctor_int_extra_roundtrip_spec_9_8_2362) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Widget(id: int): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public Extra: int := 0; "
        "  public constructor(id: int, extra: int) "
        "    this(id); "
        "    Extra := extra; "
        "  end constructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(42, 99); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget.Deserialize(d); "
        "  if w2.Id <> 42 then return 91; end; "
        "  return (if w2.Extra = 99 then 42 else 92); "
        "end Main; end C;"), 42);
}

// Repeated round-trips in a loop (50 allocate/serialize/deserialize/dispose
// cycles through the primary+explicit ctor chain) — a per-iteration leak in
// the string Extra (e.g. a missed release of the binder's +1 reference when
// the explicit ctor retains it into the field) would surface as a large,
// clearly-nonzero ASan/LSan report rather than a single missed free masked
// by the allocator noise floor. Mirrors the primary-ctor-only
// repeated_roundtrip_no_accumulating_leak test above, for the mixed path.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           primary_plus_explicit_ctor_repeated_roundtrip_no_leak_spec_9_8_2362) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Widget(id: int): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public Extra: string := \"\"; "
        "  public constructor(id: int, extra: string) "
        "    this(id); "
        "    Extra := extra; "
        "  end constructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var total: int := 0; "
        "  var i: int := 0; "
        "  while i < 50 do "
        "    using var w := new Widget(i, \"item\"); "
        "    using var s := new BinarySerializer(); "
        "    w.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    using var w2 := Widget.Deserialize(d); "
        "    total := total + w2.Id; "
        "    i := i + 1; "
        "  end; "
        // sum 0..49 = 1225; assert the exact accumulator to prove every
        // iteration's Id round-tripped through the primary+explicit chain.
        "  return (if total = 1225 then 42 else total); "
        "end Main; end C;"), 42);
}

// Adversarial self-pass: when the primary ctor DOES bijection-match the
// component set (primaryCtorUsed=true) AND a non-matching explicit ctor also
// exists, the primary-ctor path runs and the explicit ctor is NOT selected
// (the `if (!primaryCtorUsed)` gate at LlvmBackend_Serialization.cpp skips
// the explicit-ctor search). This pins that the presence of a chaining
// explicit ctor does not derail the primary-ctor path when the primary is the
// match — the two paths are mutually exclusive, and #2362's mixed case is the
// ONLY shape that reaches the explicit-ctor branch for a primary-ctor class.
BEZEL_TEST(codegen_class_primary_ctor_serialization_2325,
           primary_ctor_match_with_extra_explicit_ctor_uses_primary_spec_9_8_2362) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        // Primary ctor (id) bijection-matches the single component {id}; the
        // explicit ctor (id, note) has 2 params vs 1 component, cannot match,
        // and exists only to confirm it does not disturb the primary path.
        "class Widget(id: int): ISerializable, IDeserializable "
        "  public property Id: int => id; "
        "  public constructor(id: int, note: string) "
        "    this(id); "
        "  end constructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(5); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget.Deserialize(d); "
        "  return (if w2.Id = 5 then 42 else 91); "
        "end Main; end C;"), 42);
}
