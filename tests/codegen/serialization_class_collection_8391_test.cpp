// Codegen tests — §9.8 v0.357 serialization synthesis for a CLASS component
// whose type is a standard sequence collection (issue #8391).
//
// §9.8 v0.357's admissible-component list reads: "a standard sequence
// collection (`Array<T>`, `OwningArray<T>`, `LinkedList<T>`, etc., and inline
// arrays `[N]T`) whose element type is admissible as a serialized component
// (encoded with `BeginArray`/`EndArray`)", and — of the admissible rows — "the
// heap collections (sequence and dictionary alike, which are class types,
// §3.1) are admissible in **class** components only; for records see the
// record clause below". The class clause says the same: "A class is
// serialization-eligible when every serialized component's type is admissible
// as a serialized component (recursively admitting other conforming classes,
// `Shared<T>`/`Weak<T>` of an eligible class `T`, and owned collections of
// eligible elements)."
//
// The heap collections are CLASS components only, so the RECORD half is
// unchanged and stays pinned here: `record Bag(Items: Array<int>)` is still
// not serialization-eligible ("A record with a class-, interface-,
// `Shared<T>`-, `Weak<T>`-, or heap-collection-typed serialized component is
// not serialization-eligible — a compile error at the conformance
// declaration, naming the field").
//
// The CLASS half is what this file covers: a class that owns a collection
// field. Before the fix, `isSerializationEligibleType` had no branch for the
// heap collections at all — a builtin collection type (declaration == null)
// fell past the `Shared<T>`/`Weak<T>` name check into `if (!ts) return false`
// — so the conformance declaration was rejected with "field `Tags` has a type
// that is not serialization-eligible" (issue #8391's own observation).
//
// The fix admits exactly the shapes §9.8 admits and no more; the rejected
// shapes are pinned below, each with the reason:
//
//   * a NON-owned collection field — §9.8's class clause admits "owned
//     collections of eligible elements", and the synthesized `Deserialize`
//     constructs the collection fresh, so a borrowing collection field has no
//     owner to free it. The rule is scoped to a class that SYNTHESIZES a
//     `Deserialize`: a write-only class (one that declares only
//     `ISerializable`) or one that hand-writes its own `Deserialize` never
//     has a collection constructed for it, so it is not affected (the
//     `write_only_class_borrowed_collection_field_admitted` row below);
//   * a collection AUTO-PROPERTY — Appendix A.4's `PropertyDecl` production
//     carries no `owned` slot, so an auto-property's backing field is never
//     owned;
//   * a NESTED collection element (`Array<Array<int>>`) — the borrowing
//     outer array borrows the inner one, which the reader constructs fresh;
//   * a NULLABLE-`string` element — the collection's element slot does not
//     co-own the nullable form of a runtime-managed string;
//   * a CLASS element in a BORROWING array — the array's element slot
//     borrows a class-typed element, which the reader constructs fresh;
//   * a heap collection in a RECORD — §9.8's record clause.
//
// BinarySerializer/BinaryDeserializer and JsonSerializer/JsonDeserializer
// live in System.Serialization (§2.15) and are NOT auto-imported; the tests
// include "import System.Serialization;".

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

// runAotChecked compiles the program with the AddressSanitizer pass over the
// GENERATED module and links it against the ASan runtime, so a leak in the
// synthesized Deserialize (the ownership bug this fix is about) fails the row
// at the leak, not merely at a hand-written assertion. `ran == false` covers
// a sanitizer fire as well as a compile rejection.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The issue's own repro, verbatim, plus the stdout it is expected to print.
std::string runStdout(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}

bool compileFails(const std::string& src) {
    auto r = runAot(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

// A rejected shape, with the reason the diagnostic must name.
bool compileFailsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (!r.diagnostics.hasErrors() || r.ran) return false;
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message;
    return all.find(needle) != std::string::npos;
}

// Preamble for all serialization tests (§2.15: requires the import).
const char* kImport = "import System.Serialization; ";

} // namespace

// ===========================================================================
// The issue's own repro — §9.8 v0.357's class clause admits a heap
// collection whose element type is admissible; `string` is.
// ===========================================================================

// Issue #8391's program, verbatim: a class declaring `ISerializable` that
// owns an `Array<string>` field. Before the fix the conformance declaration
// was rejected ("class `Person` declares `ISerializable`/`IDeserializable`
// but is not serialization-eligible (§9.8): field `Tags` has a type that is
// not serialization-eligible"); after the fix it compiles and prints the
// collection's Count, which is 0.
//
// This row is ALSO the regression's trigger for the second half of the fix:
// the class declares ONLY `ISerializable`, so it synthesizes no `Deserialize`
// and §9.8's *Constructor selection* paragraph has nothing to decide for it —
// but the constructor-selection check fired anyway (it was keyed on "no
// hand-written `Deserialize`" rather than on "a `Deserialize` is synthesized")
// and rejected the class with a diagnostic naming `IDeserializable`, an
// interface the class does not declare.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owned_array_string_field_eligible_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(runStdout(
        "module P; "
        "public class Person: ISerializable "
        "  public property Name: string := \"\"; "
        "  public owned Tags: Array<string> := new Array<string>(); "
        "end Person; "
        "public class Program "
        "  public static method Main() "
        "    using var p := new Person(); "
        "    Console.WriteLine(p.Tags.Count); "
        "  end Main; "
        "end Program;"),
        "0\n");
}

// ===========================================================================
// Round trip — §9.8's "encoded with `BeginArray`/`EndArray`" (both
// concrete formats of stdlib §2.15).
// ===========================================================================

// A class whose `owned` collection field is written by the synthesized
// Serialize and read back by the synthesized Deserialize, over the BINARY
// format. The class shape is the stdlib §2.14 `Order` shape (a primary
// constructor whose parameter feeds an explicit `owned` field), so the
// synthesized Deserialize's constructor selection binds the collection
// component the way §9.8's *Constructor selection* paragraph requires.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owned_array_string_roundtrip_binary_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Bag(id: int, owned tags: Array<string>): ISerializable, IDeserializable "
        "  public owned Tags: Array<string> := tags; "
        "  public property Id: int => id; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(7, new Array<string>()); "
        "  b.Tags.Add(\"alpha\"); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Bag.Deserialize(d); "
        "  if b2.Id <> 7 then return 91; end; "
        "  if b2.Tags.Count <> 1 then return 92; end; "
        "  return (if b2.Tags[0] = \"alpha\" then 42 else 93); "
        "end Main; end C;"), 42);
}

// The same round trip over the JSON format. The two formats differ in how
// the array's extent is framed (stdlib §2.15: the binary format writes the
// count, the JSON format answers `-1` from BeginArray and is driven by
// HasMoreElements), so the read half is exercised twice.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owned_array_string_roundtrip_json_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Bag(id: int, owned tags: Array<string>): ISerializable, IDeserializable "
        "  public owned Tags: Array<string> := tags; "
        "  public property Id: int => id; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(7, new Array<string>()); "
        "  b.Tags.Add(\"alpha\"); "
        "  using var s := new JsonSerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Bag.Deserialize(d); "
        "  if b2.Id <> 7 then return 91; end; "
        "  if b2.Tags.Count <> 1 then return 92; end; "
        "  return (if b2.Tags[0] = \"alpha\" then 42 else 93); "
        "end Main; end C;"), 42);
}

// The element type is what §9.8's row makes admissible ("whose element type
// is admissible as a serialized component"), so an int element round-trips
// on the same shape.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owned_array_int_roundtrip_binary_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Bag(id: int, owned tags: Array<int>): ISerializable, IDeserializable "
        "  public owned Tags: Array<int> := tags; "
        "  public property Id: int => id; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(1, new Array<int>()); "
        "  b.Tags.Add(5); "
        "  b.Tags.Add(6); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Bag.Deserialize(d); "
        "  if b2.Tags.Count <> 2 then return 92; end; "
        "  return (if b2.Tags[0] = 5 then 42 else 93); "
        "end Main; end C;"), 42);
}

// A nested serialization-eligible RECORD is an admissible element type too
// (§9.8: "another serialization-eligible record that itself declares
// `ISerializable`/`IDeserializable`"), and the record's own synthesized
// Serialize/Deserialize is what encodes the element.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owned_array_record_roundtrip_binary_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "public record Pt(X: int, Y: int): ISerializable, IDeserializable; "
        "class Bag(id: int, owned tags: Array<Pt>): ISerializable, IDeserializable "
        "  public owned Tags: Array<Pt> := tags; "
        "  public property Id: int => id; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(1, new Array<Pt>()); "
        "  b.Tags.Add(new Pt(3, 4)); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Bag.Deserialize(d); "
        "  if b2.Tags.Count <> 1 then return 92; end; "
        "  return (if b2.Tags[0].X = 3 then 42 else 93); "
        "end Main; end C;"), 42);
}

// The OWNING family: §9.8's row names `OwningArray<T>` beside `Array<T>`.
// Its element slot owns a CLASS-typed element (stdlib §4.5 / §28.11), so a
// class element round-trips through it, and the owning collection disposes
// the elements it read.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owning_array_class_element_roundtrip_binary_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Node(label: string): ISerializable, IDeserializable "
        "  public Label: string := label; "
        "end Node; "
        "class Bag(id: int, owned tags: OwningArray<Node>): ISerializable, IDeserializable "
        "  public owned Tags: OwningArray<Node> := tags; "
        "  public property Id: int => id; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(3, new OwningArray<Node>()); "
        "  b.Tags.Add(new Node(\"n1\")); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Bag.Deserialize(d); "
        "  if b2.Id <> 3 then return 91; end; "
        "  if b2.Tags.Count <> 1 then return 92; end; "
        "  return (if b2.Tags[0].Label = \"n1\" then 42 else 93); "
        "end Main; end C;"), 42);
}

// An enum element is admissible too (§9.8: "an enum"), and the enum's own
// synthesized Serialize writes the member's name.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owned_array_enum_element_roundtrip_binary_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "enum Color: int, ISerializable, IDeserializable "
        "  Red := 1, Green := 2; "
        "end Color; "
        "class Bag(id: int, owned tags: Array<Color>): ISerializable, IDeserializable "
        "  public owned Tags: Array<Color> := tags; "
        "  public property Id: int => id; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(1, new Array<Color>()); "
        "  b.Tags.Add(Color.Green); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Bag.Deserialize(d); "
        "  if b2.Tags.Count <> 1 then return 92; end; "
        "  return (if b2.Tags[0] = Color.Green then 42 else 93); "
        "end Main; end C;"), 42);
}

// The same binary round trip under AddressSanitizer: the synthesized
// Deserialize constructs the collection fresh, so a collection the class does
// not own — or elements it does not co-own — would leak here.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owned_array_string_roundtrip_binary_leak_free_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Bag(id: int, owned tags: Array<string>): ISerializable, IDeserializable "
        "  public owned Tags: Array<string> := tags; "
        "  public property Id: int => id; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(7, new Array<string>()); "
        "  b.Tags.Add(\"alpha\"); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Bag.Deserialize(d); "
        "  if b2.Tags.Count <> 1 then return 92; end; "
        "  return (if b2.Tags[0] = \"alpha\" then 42 else 93); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// The RECORD half is unchanged — §9.8's record clause keeps the heap
// collections out of a record's serialized set.
// ===========================================================================

// §9.8's record clause: "A record with a class-, interface-, `Shared<T>`-,
// `Weak<T>`-, or heap-collection-typed serialized component is not
// serialization-eligible — a compile error at the conformance declaration,
// naming the field." This is the admitting twin of the class rows above: the
// same collection type, in the position §9.8 says is NOT admitted. It is
// green on the base and must stay green on the fix's tip.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           record_heap_collection_component_still_rejected_spec_9_8_v0357) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "public record Bag(Items: Array<int>): ISerializable, IDeserializable; "));
}

// ===========================================================================
// The shapes §9.8 does NOT admit stay rejected — the fix widens the
// eligible set by exactly the class clause's "owned collections of eligible
// elements", not beyond it.
// ===========================================================================

// A NON-owned collection field. §9.8's class clause admits "owned
// collections of eligible elements"; the synthesized `Deserialize` constructs
// the collection fresh, so a borrowing collection field would have no owner
// to free it (measured on the pre-rule build: 330 bytes in 6 allocations
// leaked per round-trip under ASan/LSan).
//
// The rejection is asserted as a REJECTION only (the specific reason is
// pinned by the `..._diagnostic_names_the_owned_rule_...` row below), so this
// row is green on the pre-fix base as well — the base rejected EVERY
// collection component — and stays green on the fix's tip.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           non_owned_collection_field_still_rejected_spec_9_8_v0357) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Bag(id: int, tags: Array<string>): ISerializable, IDeserializable "
        "  public Tags: Array<string> := tags; "
        "  public property Id: int => id; "
        "end Bag; "));
}

// The REASON the non-owned collection field is rejected: §9.8's class clause
// admits "owned collections of eligible elements". This row is RED on the
// pre-fix base (whose diagnostic was the generic "has a type that is not
// serialization-eligible") and GREEN on the fix's tip.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           non_owned_collection_field_diagnostic_names_the_owned_rule_spec_9_8_v0357) {
    BEZEL_EXPECT_TRUE(compileFailsWith(
        std::string("module M; ") + kImport +
        "class Bag(id: int, tags: Array<string>): ISerializable, IDeserializable "
        "  public Tags: Array<string> := tags; "
        "  public property Id: int => id; "
        "end Bag; ",
        "is a collection-typed field that is not `owned`"));
}

// A collection AUTO-PROPERTY. Appendix A.4's `PropertyDecl` production has
// no `owned` slot, so an auto-property's backing field is never owned and
// cannot be an "owned collection of eligible elements".
BEZEL_TEST(codegen_serialization_class_collection_8391,
           collection_auto_property_still_rejected_spec_9_8_v0357) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Bag: ISerializable, IDeserializable "
        "  public property Tags: Array<string>; "
        "end Bag; "));
}

// The REASON the collection auto-property is rejected — the same row pair as
// the non-owned field above.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           collection_auto_property_diagnostic_names_the_owned_rule_spec_9_8_v0357) {
    BEZEL_EXPECT_TRUE(compileFailsWith(
        std::string("module M; ") + kImport +
        "class Bag: ISerializable, IDeserializable "
        "  public property Tags: Array<string>; "
        "end Bag; ",
        "is a collection-typed auto-property"));
}

// A NESTED collection element. The borrowing outer array borrows the inner
// one, which the reader constructs fresh — so the inner collection would
// have no owner.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           nested_collection_element_still_rejected_spec_9_8_v0357) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Bag(id: int, owned tags: Array<Array<int>>): ISerializable, IDeserializable "
        "  public owned Tags: Array<Array<int>> := tags; "
        "  public property Id: int => id; "
        "end Bag; "));
}

// A NULLABLE-`string` element. The collection's element slot does not
// co-own the nullable form of a runtime-managed string, so the string the
// reader produces would have no owner.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           nullable_string_element_still_rejected_spec_9_8_v0357) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Bag(id: int, owned tags: Array<string?>): ISerializable, IDeserializable "
        "  public owned Tags: Array<string?> := tags; "
        "  public property Id: int => id; "
        "end Bag; "));
}

// A CLASS element in a BORROWING array. The array's element slot borrows a
// class-typed element, so the instance the reader constructs would have no
// owner (the OWNING family, `OwningArray<T>`, is the one that owns it — the
// admitted twin is the `owning_array_class_element_roundtrip_binary` row
// above).
BEZEL_TEST(codegen_serialization_class_collection_8391,
           class_element_in_borrowing_array_still_rejected_spec_9_8_v0357) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Node(label: string): ISerializable, IDeserializable "
        "  public Label: string := label; "
        "end Node; "
        "class Bag(id: int, owned tags: Array<Node>): ISerializable, IDeserializable "
        "  public owned Tags: Array<Node> := tags; "
        "  public property Id: int => id; "
        "end Bag; "));
}

// ===========================================================================
// The fix touches only the CONFORMANCE path — a class that owns a
// collection field and declares no serialization interface is unaffected.
// ===========================================================================

BEZEL_TEST(codegen_serialization_class_collection_8391,
           class_collection_field_without_conformance_unaffected_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; "
        "public class Person "
        "  public property Name: string := \"\"; "
        "  public owned Tags: Array<string> := new Array<string>(); "
        "end Person; "
        "public class Program "
        "  public static method Main() "
        "    using var p := new Person(); "
        "    Console.WriteLine(p.Tags.Count); "
        "  end Main; "
        "end Program;"),
        "0\n");
}

// ===========================================================================
// The owned-collection rule is scoped to the SYNTHESIZED `Deserialize` —
// a write-only class (one that declares only `ISerializable`) synthesizes
// no `Deserialize`, so no fresh collection is ever bound to the field.
// ===========================================================================

// The rule's rationale is entirely about the compiler CONSTRUCTING the
// collection: "the synthesized `Deserialize` constructs the collection
// fresh, so a borrowing collection field would have no owner to free it".
// A class that declares ONLY `ISerializable` synthesizes NO `Deserialize`,
// so that argument does not apply — and the class is not rejected. (The
// field borrows the collection its constructor parameter names, which is
// the only shape a NON-`owned` collection field can hold: §28.5 rejects a
// fresh production stored into a non-`owned` field.)
//
// (This row is RED on the pre-fix tip: the write-only class with a borrowed
// `Array<string>` field was rejected with the owned-collection diagnostic.)
BEZEL_TEST(codegen_serialization_class_collection_8391,
           write_only_class_borrowed_collection_field_admitted_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Bag(tags: Array<string>): ISerializable "
        "  public Tags: Array<string> := tags; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"x\"); "
        "  using var b := new Bag(a); "
        "  using var s := new JsonSerializer(); "
        "  b.Serialize(s); "
        "  using var jb := s.ToBytes(); "
        "  return (if jb.Length > 0 then 42 else 1); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// The ERROR PATH — a truncated wire must raise SerializationException and
// leave NO heap leak (§28.3's exception-path discharge: "Exceptions
// therefore do not produce leaks"; the pinned repo standard is
// tests/codegen/serialization_tier2_classes_test.cpp — "truncated binary
// input must raise SerializationException and leave no heap leak or
// double-free").
//
// The reader allocates the collection BEFORE it reads the elements, so a
// truncated element stream leaves a half-built array (and the elements
// already added) with no owner unless the unwind edge discharges it.
// ===========================================================================

// A class whose ONLY component is the collection: every truncation lands
// inside the array read, so the half-built array is the thing that must be
// discharged. Under AddressSanitizer with leak detection this row fails on
// the pre-fix build (the array — and the elements already added to it —
// leak) and passes on the fix's tip.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           truncated_array_input_raises_and_leaves_no_leak_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Bag(owned tags: Array<string>): ISerializable, IDeserializable "
        "  public owned Tags: Array<string> := tags; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(new Array<string>()); "
        "  b.Tags.Add(\"alpha\"); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var caught: bool := false; "
        "  var cut: int := bytes.Length - 1; "
        "  while (not caught) and (cut > 0) do "
        "    try "
        "      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cut)); "
        "      using var back := Bag.Deserialize(d); "
        "    catch SerializationException => caught := true; "
        "    end; "
        "    cut := cut - 1; "
        "  end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

// The SAME standard for the OTHER error edge: the collection is read
// COMPLETELY and a LATER component's read throws. The class instance does
// not exist yet at that point (it is constructed after every component is
// read), so the class's own owned-field cascade cannot discharge the
// collection — the deserializer's component scope must.
//
// The components are declared collection-FIRST and int-LAST, so the wire
// carries the array before the int and a cut at the tail truncates the int
// while the array has already been read whole.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           truncated_later_component_raises_and_leaves_no_leak_spec_9_8_v0357) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Bag(owned tags: Array<string>, n: int): ISerializable, IDeserializable "
        "  public owned Tags: Array<string> := tags; "
        "  public N: int := n; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag(new Array<string>(), 7); "
        "  b.Tags.Add(\"alpha\"); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var caught: bool := false; "
        "  var cut: int := bytes.Length - 1; "
        "  while (not caught) and (cut > 0) do "
        "    try "
        "      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cut)); "
        "      using var back := Bag.Deserialize(d); "
        "    catch SerializationException => caught := true; "
        "    end; "
        "    cut := cut - 1; "
        "  end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}
