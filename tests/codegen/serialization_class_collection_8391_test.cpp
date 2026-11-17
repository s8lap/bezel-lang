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

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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
// The REASON the shapes §9.8 does NOT admit are rejected. The bare
// "still rejected" programs (the record heap-collection component, the nested
// collection element, the nullable-string element, the class element in a
// borrowing array, the collection auto-property, the non-owned field, and the
// unaffected no-conformance class) are green on the pre-fix base as well, so
// they are conformance-corpus cases, not gtest rows (owner rule 2026-11-12;
// the programs are kept as corpus candidates for the harvest lane).
// ===========================================================================

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

// ===========================================================================
// THE EXCEPTION PATH OF AN ADMITTED COLLECTION COMPONENT (#8391 review F6,
// and the F1 rows the first review asked for).
//
// §9.8 v0.357: "A **missing** required component (one neither present nor
// `serialized optional`), a **duplicate** field name, a type mismatch, or a
// name not matching any enum constant raises `SerializationException`
// unconditionally" — so a truncated payload raises. §28.3 v0.357: "**Owned**
// — the holder is responsible for disposing the referenced object exactly
// once": what the reader has built when it raises is disposed once — not
// leaked (F1), and not disposed twice (F6).
//
// F6. The element reader of an `OwningArray<Class>` component is ONE emitted
// copy of the element class's synthesized `Deserialize`, run once per element
// in the collection reader's loop. Before the #8574 fix its component slots
// were reset once per function, so element k+1 began with element k's
// pointers still in them — pointers element k's object already owned. A cut
// inside element 2 or 3 then made the unwind sweep release element 1's or 2's
// string again (`refcount over-release (string)`), or free its owned
// `Array<string>` twice (AddressSanitizer double-free). A SINGLE `Deserialize`
// call is enough; the rows marked "one call per frame" read in a helper
// method so that nothing but the element loop can carry a pointer over.
//
// Every row here runs under `runAotChecked` (AddressSanitizer over the
// generated module, LeakSanitizer at exit), drives each failing read at least
// 200 times, and asserts `didNotCrash` and `assertNoLeak`.
// ===========================================================================

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
void didNotCrash(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, const std::string& out) {
    didNotCrash(r, out);
    assertNoLeak(r);
}

// A shape is the type declarations and the statements that bind `src`, the
// `P` to serialize.
struct Shape {
    const char* decls;
    const char* build;
};

const char* kHead = "module M; import System.Serialization;\n";

// Three `Leaf(name: string, v: int)` elements — review F6's first shape
// (the reviewer's sw4 sweep: cuts 48..65 of this payload failed).
const Shape kOwningLeaves = {
    R"BZL(class Leaf(name: string, v: int): ISerializable, IDeserializable
  public Name: string := name;
  public property V: int => v;
end Leaf;
class P(id: int, owned items: OwningArray<Leaf>): ISerializable, IDeserializable
  public owned Items: OwningArray<Leaf> := items;
  public property Id: int => id;
end P;)BZL",
    R"BZL(using var src := new P(3, new OwningArray<Leaf>());
    src.Items.Add(new Leaf("a" + "a", 1)); src.Items.Add(new Leaf("b" + "b", 2)); src.Items.Add(new Leaf("c" + "c", 3));)BZL"};

// Three `Node(label: string, owned subs: Array<string>)` elements — review
// F6's second shape (the reviewer's t3_owningnode_bin sweep: 83 cuts failed).
const Shape kOwningNodes = {
    R"BZL(class Node(label: string, owned subs: Array<string>): ISerializable, IDeserializable
  public Label: string := label;
  public owned Subs: Array<string> := subs;
end Node;
class P(id: int, owned tags: OwningArray<Node>): ISerializable, IDeserializable
  public owned Tags: OwningArray<Node> := tags;
  public property Id: int => id;
end P;)BZL",
    R"BZL(using var src := new P(3, new OwningArray<Node>());
    var bi := 0;
    while bi < 3 do
      var subs := new Array<string>(); subs.Add("s" + bi.ToString()); subs.Add("t" + bi.ToString());
      src.Tags.Add(new Node("n" + bi.ToString(), move subs));
      bi := bi + 1;
    end;)BZL"};

const Shape kArrayOfString = {
    R"BZL(class P(id: int, owned tags: Array<string>, s: string): ISerializable, IDeserializable
  public owned Tags: Array<string> := tags;
  public property Id: int => id;
  public S: string := s;
end P;)BZL",
    R"BZL(using var src := new P(7, new Array<string>(), "ta" + "il");
    src.Tags.Add("a" + "a"); src.Tags.Add("b" + "b");)BZL"};

const Shape kNullableArray = {
    R"BZL(class P(id: int, owned tags: Array<string>?, s: string): ISerializable, IDeserializable
  public owned Tags: Array<string>? := tags;
  public property Id: int => id;
  public S: string := s;
end P;)BZL",
    R"BZL(using var src := new P(7, new Array<string>(), "ta" + "il");
    src.Tags!.Add("a" + "a");)BZL"};

// ONE CALL PER FRAME, every prefix: `One` holds the only `P.Deserialize(d)`
// and is entered afresh for each read, so the only loop the emitted reader
// runs in is the collection component's own element loop. 200 rounds of
// cut = 0, 1, .. n; a strict prefix raises, the whole payload round-trips.
std::string oneCallPerFrameEveryPrefix(const Shape& sh) {
    return std::string(kHead) + sh.decls + R"BZL(
class C
  static method One(bytes: OwningBuffer<byte>, cut: int): int
    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cut));
    try
      using var x := P.Deserialize(d);
      return 1;
    catch SerializationException => return 0;
    end;
  end One;
  public static method Main(): int
    )BZL" + sh.build + R"BZL(
    using var s := new BinarySerializer();
    src.Serialize(s);
    using var bytes := s.ToBytes();
    var n := bytes.Length;
    var caught := 0;
    var ok := 0;
    var round := 0;
    while round < 200 do
      var cut := 0;
      while cut <= n do
        if One(bytes, cut) = 1 then ok := ok + 1; else caught := caught + 1; end;
        cut := cut + 1;
      end;
      round := round + 1;
    end;
    Console.WriteLine(ok);
    Console.WriteLine(caught = 200 * n);
    return 42;
  end Main;
end C;
)BZL";
}

// THE CALL LOOP, every prefix: the read sits in `Main`'s own loop (the form
// the reviewer's sweep used), so the emitted reader also runs once per call.
std::string callLoopEveryPrefix(const Shape& sh) {
    return std::string(kHead) + sh.decls + R"BZL(
class C
  public static method Main(): int
    )BZL" + sh.build + R"BZL(
    using var s := new BinarySerializer();
    src.Serialize(s);
    using var bytes := s.ToBytes();
    var n := bytes.Length;
    var caught := 0;
    var ok := 0;
    var round := 0;
    while round < 200 do
      var cut := 0;
      while cut <= n do
        using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cut));
        try
          using var x := P.Deserialize(d);
          ok := ok + 1;
        catch SerializationException => caught := caught + 1;
        end;
        cut := cut + 1;
      end;
      round := round + 1;
    end;
    Console.WriteLine(ok);
    Console.WriteLine(caught = 200 * n);
    return 42;
  end Main;
end C;
)BZL";
}

// THE CALL LOOP, every ordered pair (a, b) of cuts 0..n through one textual
// `P.Deserialize(d)`: success -> failure, failure -> success and failure ->
// failure, the failure at every offset on both sides. Each cut is driven
// 2 * (n + 1) * rounds times on each side; every row picks `rounds` so that
// is at least 200. The program checks both counts and prints two booleans.
std::string callLoopEveryPair(const Shape& sh, const char* format, int rounds) {
    const std::string r = std::to_string(rounds);
    return std::string(kHead) + sh.decls + R"BZL(
class C
  public static method Main(): int
    )BZL" + sh.build + R"BZL(
    using var s := new )BZL" + format + R"BZL(Serializer();
    src.Serialize(s);
    using var bytes := s.ToBytes();
    var n := bytes.Length;
    var caught := 0;
    var ok := 0;
    var round := 0;
    while round < )BZL" + r + R"BZL( do
      var a := 0;
      while a <= n do
        var b := 0;
        while b <= n do
          var k := 0;
          while k < 2 do
            var cut := a;
            if k = 1 then cut := b; end;
            using var d := new )BZL" + format + R"BZL(Deserializer(bytes.AsReadOnlySpan(0, cut));
            try
              using var x := P.Deserialize(d);
              ok := ok + 1;
            catch SerializationException => caught := caught + 1;
            end;
            k := k + 1;
          end;
          b := b + 1;
        end;
        a := a + 1;
      end;
      round := round + 1;
    end;
    var wantOk := )BZL" + r + R"BZL( * 2 * (n + 1);
    var wantCaught := )BZL" + r + R"BZL( * 2 * (n + 1) * (n + 1) - wantOk;
    Console.WriteLine(ok = wantOk);
    Console.WriteLine(caught = wantCaught);
    return 42;
  end Main;
end C;
)BZL";
}

// The review's loop form (probe L1 / L2), 200 calls: even calls read the
// whole payload, odd calls read 1 byte and raise before any component.
// `%T%` is the element type; `%ADD%` fills the array.
std::string wholeThenOneByte(const char* elemType, const char* add) {
    std::string p = std::string(kHead) + R"BZL(
class Bag(id: int, owned tags: Array<%T%>): ISerializable, IDeserializable
  public owned Tags: Array<%T%> := tags;
  public property Id: int => id;
end Bag;
class C
  public static method Main(): int
    using var good := new Bag(7, new Array<%T%>());
    %ADD%
    using var s := new BinarySerializer();
    good.Serialize(s);
    using var bytes := s.ToBytes();
    var caught := 0;
    var ok := 0;
    var i := 0;
    while i < 200 do
      var n := bytes.Length;
      if i mod 2 = 1 then n := 1; end;
      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, n));
      try
        using var b2 := Bag.Deserialize(d);
        ok := ok + 1;
      catch SerializationException => caught := caught + 1;
      end;
      i := i + 1;
    end;
    Console.WriteLine(ok);
    Console.WriteLine(caught);
    return 42;
  end Main;
end C;
)BZL";
    auto replaceAll = [&p](const std::string& from, const std::string& to) {
        for (std::size_t at = p.find(from); at != std::string::npos;
             at = p.find(from, at + to.size()))
            p.replace(at, from.size(), to);
    };
    replaceAll("%T%", elemType);
    replaceAll("%ADD%", add);
    return p;
}

// An `OwningArray<Meta>` whose FIRST element carries its `serialized
// optional` component and whose other two omit it, read by one call per
// frame, 200 times. §9.8 v0.357: "On read, a missing field yields
// `default(T)`" — two nulls per read. `%T%` is the component type, `%V%` the
// first element's value.
std::string optionalInElements(const char* type, const char* value) {
    std::string p = std::string(kHead) + R"BZL(
class Meta : ISerializable, IDeserializable
  public property Code: int;
  public property Note: %T%? serialized optional;
  public constructor(code: int, note: %T%?)
    Code := code; Note := note;
  end constructor;
end Meta;
class Bag(id: int, owned items: OwningArray<Meta>): ISerializable, IDeserializable
  public owned Items: OwningArray<Meta> := items;
  public property Id: int => id;
end Bag;
class C
  public static method One(): int
    using var b := new Bag(3, new OwningArray<Meta>());
    b.Items.Add(new Meta(1, %V%)); b.Items.Add(new Meta(2, null)); b.Items.Add(new Meta(3, null));
    using var s := new BinarySerializer();
    b.Serialize(s);
    using var bytes := s.ToBytes();
    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan());
    using var b2 := Bag.Deserialize(d);
    var nulls := 0;
    var k := 0;
    while k < b2.Items.Count do
      if b2.Items[k].Note = null then nulls := nulls + 1; end;
      k := k + 1;
    end;
    return nulls;
  end One;
  public static method Main(): int
    var i := 0;
    var nulls := 0;
    while i < 200 do
      nulls := nulls + One();
      i := i + 1;
    end;
    Console.WriteLine(nulls);
    return 42;
  end Main;
end C;
)BZL";
    auto replaceAll = [&p](const std::string& from, const std::string& to) {
        for (std::size_t at = p.find(from); at != std::string::npos;
             at = p.find(from, at + to.size()))
            p.replace(at, from.size(), to);
    };
    replaceAll("%T%", type);
    replaceAll("%V%", value);
    return p;
}

}  // namespace

// --- F6: one call per frame --------------------------------------------------

// `OwningArray<Leaf(name: string, v: int)>`, three elements, binary, every
// prefix (the reviewer's sw4 cuts 48..65 among them), one call per frame.
// Pre-fix: `bezel: fatal: refcount over-release (string)`.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owning_array_of_string_class_one_call_every_prefix_200_rounds_spec_9_8_v0357) {
    expectClean(runAotChecked(oneCallPerFrameEveryPrefix(kOwningLeaves)),
                "200\nTrue\n");
}

// `OwningArray<Node(label: string, owned subs: Array<string>)>`, three
// elements, binary, every prefix, one call per frame. Pre-fix:
// AddressSanitizer attempting double-free (the previous element's
// `Array<string>`).
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owning_array_of_array_owning_class_one_call_every_prefix_200_rounds_spec_9_8_v0357) {
    expectClean(runAotChecked(oneCallPerFrameEveryPrefix(kOwningNodes)),
                "200\nTrue\n");
}

// --- F6 in the reviewer's own form: the read in a loop -----------------------

// The reviewer's t3_owningnode_bin sweep as one program: every prefix, 200
// rounds, the read in `Main`'s loop.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owning_array_of_array_owning_class_call_loop_every_prefix_200_rounds_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPrefix(kOwningNodes)),
                "200\nTrue\n");
}

// The same shape in JSON, every ordered pair of cuts.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owning_array_of_array_owning_class_every_ordered_pair_of_cuts_json_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kOwningNodes, "Json", 1)),
                "True\nTrue\n");
}

// `OwningArray<Leaf>` in binary, every ordered pair of cuts.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           owning_array_of_string_class_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kOwningLeaves, "Binary", 1)),
                "True\nTrue\n");
}

// --- The review's loop form: the collection slot itself ----------------------

// Probe L1: `Array<int>`, the whole payload then one byte, 200 calls.
// Pre-fix: AddressSanitizer attempting double-free (the previous call's
// array, still in the component slot).
BEZEL_TEST(codegen_serialization_class_collection_8391,
           array_of_int_whole_then_one_byte_200_calls_spec_9_8_v0357) {
    expectClean(runAotChecked(wholeThenOneByte(
                    "int", "good.Tags.Add(1); good.Tags.Add(2);")),
                "100\n100\n");
}

// Probe L2: the same with `Array<string>`.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           array_of_string_whole_then_one_byte_200_calls_spec_9_8_v0357) {
    expectClean(runAotChecked(wholeThenOneByte(
                    "string",
                    "good.Tags.Add(\"al\" + \"pha\"); good.Tags.Add(\"be\" + \"ta\");")),
                "100\n100\n");
}

// `Array<string>` followed by a string, every ordered pair of cuts — binary
// and JSON.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           array_of_string_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kArrayOfString, "Binary", 2)),
                "True\nTrue\n");
}

BEZEL_TEST(codegen_serialization_class_collection_8391,
           array_of_string_every_ordered_pair_of_cuts_json_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kArrayOfString, "Json", 3)),
                "True\nTrue\n");
}

// A NULLABLE collection component, every ordered pair of cuts.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           nullable_array_of_string_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kNullableArray, "Binary", 2)),
                "True\nTrue\n");
}

// --- `serialized optional` inside an element ---------------------------------

// `int?`: a WRONG-OUTPUT row. Pre-fix the two elements that omit `Note` read
// back the first element's 77, so the program printed 0 — with no sanitizer
// report at all.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           optional_int_omitted_in_later_elements_reads_null_spec_9_8_v0357) {
    expectClean(runAotChecked(optionalInElements("int", "77")), "400\n");
}

// `string?`: pre-fix the two later elements aliased the first element's
// string without a reference of their own — `refcount over-release (string)`
// on a read that SUCCEEDS.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           optional_string_omitted_in_later_elements_reads_null_spec_9_8_v0357) {
    expectClean(runAotChecked(optionalInElements("string", "\"a\" + \"a\"")),
                "400\n");
}

// --- F1: the exception rows the first review asked for (probes a6, g1, a7) ---
//
// They pin #8391's own unwind cleanup of the half-built collection, 200
// times over and under the leak oracle. As single reads all three were
// already clean at the #8391 commit; g1 and a7 below still are, and are the
// controls of this group. a6, driven here as 200 rounds over ONE call site,
// also runs into #8574's stale slots and is red without that fix.

// Probe a6: `Array<string>` of three, every cut 1..39, 200 rounds — 39 raises
// per round.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           array_of_string_cuts_1_to_39_200_rounds_no_leak_spec_9_8_v0357) {
    expectClean(runAotChecked(std::string(kHead) + R"BZL(
class Bag(id: int, owned tags: Array<string>): ISerializable, IDeserializable
  public owned Tags: Array<string> := tags;
  public property Id: int => id;
end Bag;
class C
  public static method Main(): int
    var caught := 0;
    var round := 0;
    while round < 200 do
      var cut := 1;
      while cut < 40 do
        using var b := new Bag(7, new Array<string>());
        b.Tags.Add("alpha"); b.Tags.Add("beta"); b.Tags.Add("gamma");
        using var s := new BinarySerializer();
        b.Serialize(s);
        using var bytes := s.ToBytes();
        if cut < bytes.Length then
          using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cut));
          try
            using var b2 := Bag.Deserialize(d);
          catch SerializationException => caught := caught + 1;
          end;
        end;
        cut := cut + 1;
      end;
      round := round + 1;
    end;
    Console.WriteLine(caught);
    return 42;
  end Main;
end C;
)BZL"),
                "7800\n");
}

// Probe g1: `Array<int>` of three, cut 2 bytes short of the end — the count
// is read and the last element is missing. 200 calls.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           array_of_int_last_element_missing_200_calls_no_leak_spec_9_8_v0357) {
    expectClean(runAotChecked(std::string(kHead) + R"BZL(
class Bag(id: int, owned tags: Array<int>): ISerializable, IDeserializable
  public owned Tags: Array<int> := tags;
  public property Id: int => id;
end Bag;
class C
  public static method Main(): int
    using var b := new Bag(7, new Array<int>());
    b.Tags.Add(1); b.Tags.Add(2); b.Tags.Add(3);
    using var s := new BinarySerializer();
    b.Serialize(s);
    using var bytes := s.ToBytes();
    var caught := 0;
    var i := 0;
    while i < 200 do
      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, bytes.Length - 2));
      try
        using var b2 := Bag.Deserialize(d);
      catch SerializationException => caught := caught + 1;
      end;
      i := i + 1;
    end;
    Console.WriteLine(caught);
    return 42;
  end Main;
end C;
)BZL"),
                "200\n");
}

// Probe a7: a hand-built payload whose array count is 2^31-1 with no
// elements behind it. 200 calls.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           array_count_without_elements_200_calls_no_leak_spec_9_8_v0357) {
    expectClean(runAotChecked(std::string(kHead) + R"BZL(
class Bag(id: int, owned tags: Array<string>): ISerializable, IDeserializable
  public owned Tags: Array<string> := tags;
  public property Id: int => id;
end Bag;
class C
  public static method Main(): int
    using var s := new BinarySerializer();
    s.BeginObject(2);
    s.WriteFieldName("id"); s.WriteInt(7);
    s.WriteFieldName("Tags"); s.BeginArray(2147483647);
    using var bytes := s.ToBytes();
    var caught := 0;
    var i := 0;
    while i < 200 do
      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan());
      try
        using var b2 := Bag.Deserialize(d);
      catch SerializationException => caught := caught + 1;
      end;
      i := i + 1;
    end;
    Console.WriteLine(caught);
    return 42;
  end Main;
end C;
)BZL"),
                "200\n");
}

// ===========================================================================
// N1 of the third review (#8391): a DESERIALIZED `Array<E>` of a record with a
// string component must CO-OWN the strings its slots embed.
//
// §28.9 (v0.357): `string` is runtime-managed; §28.3.1 K1.6: a runtime-managed
// value carries no obligation, so "the stored copy stays live" is the
// runtime's job, realized by refcount held by whatever stores the copy; §28.11
// (*The element slot policy*): the borrowing `Array<T>` holds a reference on
// every runtime-managed string its slots embed. Staging's #8386 realizes that
// for `new Array<E>` by stamping the array with `bz_array_own_embedded_strings`;
// the §9.8 deserialize arm built its array with `bz_array_alloc` and did not,
// so
//     b2.Items.Add(new E(f[0], i));      // f a loop-local Array<string>
//     r.Name                             // read freed memory
// was a use-after-free on a shape the #8391 fix newly admits.
//
// Every row runs under `runAotChecked` (AddressSanitizer over the generated
// module + LeakSanitizer at exit), 100 deserialize rounds per program, and is
// RED on the pre-fix tip. The programs that are green with and without the
// stamp (the `new Array<E>` control; nullable and generic components) are
// conformance-corpus candidates, not rows here (owner rule 2026-11-12).
// ===========================================================================

namespace {

// `<ser>` / `<des>` name the format pair (Binary / Json).
std::string formatPair(std::string text, const char* fmt) {
    const std::string f = fmt;
    for (const char* tok : {"<Ser>", "<Des>"}) {
        const std::string t = tok;
        const std::string repl =
            f + (t == "<Ser>" ? "Serializer" : "Deserializer");
        for (std::size_t p; (p = text.find(t)) != std::string::npos;)
            text.replace(p, t.size(), repl);
    }
    return text;
}

// The review's probe n7: an EMPTY `Bag` round-trips, then three elements built
// from a loop-local `Split` temporary are added to the DESERIALIZED array.
const char* kAddTempStringProgram = R"BZL(
record E(Name: string, N: int): ISerializable, IDeserializable;
class Bag(id: int, owned items: Array<E>): ISerializable, IDeserializable
  public owned Items: Array<E> := items;
  public property Id: int => id;
end Bag;
class C
  public static method Main(): int
    var total := 0;
    for var k in 1..100 do
      using var b := new Bag(1, new Array<E>());
      using var s := new <Ser>();
      b.Serialize(s);
      using var bytes := s.ToBytes();
      using var d := new <Des>(bytes.AsReadOnlySpan());
      using var b2 := Bag.Deserialize(d);
      for var i in 1..3 do
        var f := $"name{i},z".Split(',');
        b2.Items.Add(new E(f[0], i));
      end;
      var junk := new Array<string>();
      for var i in 1..100 do junk.Add($"junkjunkjunk{i}"); end;
      for var r in b2.Items do total := total + r.Name.Length + r.N; end;
    end;
    Console.WriteLine(total);
    return 42;
  end Main;
end C;
)BZL";

}  // namespace

// 100 rounds x ("name1".."name3" = 5 chars each, + N 1..3) = 100 x 21.
BEZEL_TEST(codegen_serialization_class_collection_8391,
           deserialized_array_of_record_add_temp_string_binary_100x_spec_28_9_v0357) {
    expectClean(runAotChecked(std::string(kHead) +
                              formatPair(kAddTempStringProgram, "Binary")),
                "2100\n");
}

BEZEL_TEST(codegen_serialization_class_collection_8391,
           deserialized_array_of_record_add_temp_string_json_100x_spec_28_9_v0357) {
    expectClean(runAotChecked(std::string(kHead) +
                              formatPair(kAddTempStringProgram, "Json")),
                "2100\n");
}

namespace {

// The mutators over a NON-EMPTY deserialized array whose element nests a
// record and carries two strings: a temporary-built element added, an indexer
// store, a RemoveAt, a copy-out that outlives a Clear, and a re-Add — every
// path that gives a slot's string references back or takes them (§28.11
// *The element slot policy*). The same statements run against a
// `new Array<E>` (the #8386 stamp) print the same total.
const char* kMutatorsProgram = R"BZL(
record In(S: string): ISerializable, IDeserializable;
record E(A: string, N: int, I: In, B: string): ISerializable, IDeserializable;
class Bag(id: int, owned items: Array<E>): ISerializable, IDeserializable
  public owned Items: Array<E> := items;
  public property Id: int => id;
end Bag;
class C
  public static method Main(): int
    var total := 0;
    for var k in 1..100 do
      using var b := new Bag(1, new Array<E>());
      b.Items.Add(new E("a0", 0, new In("s0"), "b0"));
      using var s := new <Ser>();
      b.Serialize(s);
      using var bytes := s.ToBytes();
      using var d := new <Des>(bytes.AsReadOnlySpan());
      using var b2 := Bag.Deserialize(d);
      for var i in 1..3 do
        var f := $"a{i},s{i},b{i}".Split(',');
        b2.Items.Add(new E(f[0], i, new In(f[1]), f[2]));
      end;
      var junk := new Array<string>();
      for var i in 1..100 do junk.Add($"junkjunkjunk{i}"); end;
      for var r in b2.Items do
        total := total + r.A.Length + r.I.S.Length + r.B.Length;
      end;
      b2.Items[0] := b2.Items[2];
      b2.Items.RemoveAt(1);
      var t := b2.Items[0];
      b2.Items.Clear();
      b2.Items.Add(t);
      total := total + b2.Items[0].B.Length;
    end;
    Console.WriteLine(total);
    return 42;
  end Main;
end C;
)BZL";

// Every cut of a one-element payload (two strings in the element), 100
// rounds: the half-built array is discharged on the unwind edge, and the
// strings a stamped array co-owns are given back exactly once.
const char* kTruncationProgram = R"BZL(
record E(A: string, N: int, B: string): ISerializable, IDeserializable;
class Bag(id: int, owned items: Array<E>): ISerializable, IDeserializable
  public owned Items: Array<E> := items;
  public property Id: int => id;
end Bag;
class C
  public static method Main(): int
    var caught := 0;
    var expected := 0;
    for var k in 1..100 do
      using var b := new Bag(1, new Array<E>());
      for var i in 1..3 do
        var f := $"alpha{i},beta{i}".Split(',');
        b.Items.Add(new E(f[0], i, f[1]));
      end;
      using var s := new <Ser>();
      b.Serialize(s);
      using var bytes := s.ToBytes();
      for var cut in 1..(bytes.Length - 1) do
        expected := expected + 1;
        using var d := new <Des>(bytes.AsReadOnlySpan(0, cut));
        try
          using var b2 := Bag.Deserialize(d);
        catch SerializationException => caught := caught + 1;
        end;
      end;
    end;
    Console.WriteLine(caught = expected);
    return 42;
  end Main;
end C;
)BZL";

}  // namespace

// 100 rounds x 26 (see the program: 24 summed in the loop + 2 re-read).
BEZEL_TEST(codegen_serialization_class_collection_8391,
           deserialized_array_of_record_mutators_json_100x_spec_28_11_v0357) {
    expectClean(runAotChecked(std::string(kHead) +
                              formatPair(kMutatorsProgram, "Json")),
                "2600\n");
}

BEZEL_TEST(codegen_serialization_class_collection_8391,
           deserialized_array_of_record_every_cut_binary_100x_no_leak_spec_28_3_v0357) {
    expectClean(runAotChecked(std::string(kHead) +
                              formatPair(kTruncationProgram, "Binary")),
                "True\n");
}
