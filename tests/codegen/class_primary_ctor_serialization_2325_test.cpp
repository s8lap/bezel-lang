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
