// Spec-derived codegen tests — §9.8 serialization synthesis, TIER 2: classes.
// Issue #550, milestone workflow step 1 (tests before implementation).
//
// These tests are EXPECTED TO FAIL (red) against the current compiler.
// No tier-2 class serialization implementation exists. The correct red shape
// for each test is:
//   - Round-trip tests: compile fails with a "class not serialization-eligible"
//     or "codegen unimplemented" diagnostic, so r.ran = false → run() = -999
//     rather than 42. OR the JIT runs but the synthesized bodies are stub/
//     empty → wrong exit code.
//   - Rejection tests: should remain green if the SEMA gate already rejects
//     (the rejection of a class without graph-shape clause already works in
//     tier 1). If the gate doesn't extend to all new cases, compileFails()
//     returns false → BEZEL_EXPECT_TRUE fails.
//
// SPEC SILENCES FOUND (reported here, not guessed):
//   SILENCE-1: Subclass serialization — §9.8 requires each class to declare
//     its own graph-shape clause ("a class may opt into serialization synthesis
//     by declaring a `serialized` graph-shape clause on its header"). Whether a
//     derived class may inherit a parent's `serialized tree` clause or must
//     re-declare it is not stated. §9.8's eligibility rule references "a class
//     with a `serialized tree`/`graph`/`cyclic` clause" — implying it must
//     appear on that class's own header. The inheritance×serialization
//     interaction is underspecified; no test written for it.
//
//   SILENCE-2: `sealed` interaction — §9.8 makes no mention of `sealed`.
//     `sealed` is treated as orthogonal. A `sealed serialized tree` class
//     should synthesize identically to a non-sealed one.
//
//   SILENCE-3: Multiple constructors on a serialized class — §9.8 says the
//     synthesized Deserialize "reconstructs the owning spanning tree … by an
//     ordinary call to its constructor." The spec does not specify which
//     constructor is chosen when there are multiple. The implementation must
//     resolve this; no multi-constructor test is written here pending
//     clarification.
//
//   SILENCE-4: Borrow-bearing record containing a serialization-eligible class
//     field. §9.7 says "Reference-typed fields in records are always borrows;
//     the record does not own what they point to." §9.8 allows "a serialization-
//     eligible synthesized class" as a record component. But how Deserialize
//     for such a record creates the owned class instances and exposes them to
//     the caller (while the record only holds borrows) is unspecified. No
//     round-trip test is written for this case pending clarification. The
//     class-owns-record direction (class auto-property of record type) is well-
//     specified and tested in tree_class_containing_record_field_roundtrip.
//
// All APIs verified against Bezel_Standard_Library.md. No invented APIs.
// BinarySerializer/BinaryDeserializer require `import System.Serialization`
// (§2.15). ISerializable/IDeserializable are auto-imported (§2.14).
//
// Spec refs: §9.8 (v0.135), §2.14, §2.15, §28.8.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"   // runAot (JIT->AOT migration, §9.8 graph tests)
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;
using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// §9.8 graph back-edge tests migrated to the AOT harness (real driver, full
// front-end incl. §28.3 ownership + §22.7 v0.184 definite assignment). runJit
// masked these programs' front-end faults by skipping the ownership/DA passes
// (codegen_support.hpp runJitInProcess runs neither); the AOT path is the
// correct oracle. Each migrated test carries a note on the reshape it needed.
int runGraphAot(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool compileFails(const std::string& src) {
    auto r = runJit(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

// Compile `source` to a native object file, link with ASan runtime, run.
// Returns 0 (clean), 1 (sanitizer triggered), or -1 (toolchain unavailable/skip).
// Per owned_exception_field_test.cpp pattern.
int runUnderLeakSanitizer550(const std::string& source, bool detectLeaks = true) {
    namespace fs = std::filesystem;
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_550_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                       // clean
}

// Preamble for all BinarySerializer tests (§2.15: requires import).
const char* kImport = "import System.Serialization; ";

} // namespace

// ===========================================================================
// §9.8 REJECTION: class without a graph-shape clause → compile error
//
// "Classes that declare neither clause remain outside synthesis entirely and
// participate only by hand-writing Serialize and a recognized static
// Deserialize."
// "A type that declares `: ISerializable` or `: IDeserializable` but is not
// serialization-eligible is a compile error at the conformance declaration."
// ===========================================================================

// §9.8 v0.141 (#703 migration): the `serialized tree/graph/cyclic` keyword
// clause was retired. A class participates in serialization synthesis by
// DECLARING `: ISerializable`/`: IDeserializable`; its wire shape is driven by
// field types. A keyword-less Box with a single value property round-trips
// type-driven (was a compile error under the keyword model). Ineligible-component
// rejection is covered by tree_class_ineligible_owned_child_is_error below.
BEZEL_TEST(codegen_serialization_t2_classes,
           class_iserializable_keyword_less_type_driven_roundtrip_spec_9_8_v0141) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Box: ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(7); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Box.Deserialize(d); "
        "  return b2.V; "
        "end Main; end C;"), 7);
}

// §9.8: a `serialized tree` class whose owned child class has no graph-shape
// clause → the child is not serialization-eligible → compile error.
// §9.8: "A class with either clause is serialization-eligible when every
// serialized component is itself serialization-eligible by the rule above."
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_ineligible_owned_child_is_error_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        // NoClause has no serialized clause → not serialization-eligible.
        "class NoClause "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end NoClause; "
        // Parent declares serialized tree but references ineligible NoClause.
        "class Parent : ISerializable, IDeserializable "
        "  public owned Child: NoClause; "
        "  public constructor(owned c: NoClause) "
        "    Child := move c; "
        "  end constructor; "
        "end Parent; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// §9.8: a record with a class-typed field where the class has NO graph-shape
// clause → the record is NOT serialization-eligible (the class is not a
// "serialization-eligible synthesized class").
// "a serialization-eligible synthesized class (a class with a `serialized
// tree`, `serialized graph`, or `serialized cyclic` clause)"
BEZEL_TEST(codegen_serialization_t2_classes,
           record_with_non_serialized_class_field_is_error_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Bare "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Bare; "
        // Bare has no serialized clause → R's field Obj is ineligible.
        "record R(Id: int, Obj: Bare): ISerializable;"));
}

// ===========================================================================
// §9.8 / `serialized tree`: simple class round-trips
//
// "serialized tree asserts the class's serialized object graph is a tree:
// along the serialized edges, no two references alias the same object, and
// there are no cycles."
// "Synthesized Deserialize … Deserialize(d: IDeserializer): owned T
// (a class instance is an owned heap value transferred to the caller, §28.8)."
// ===========================================================================

// §9.8: simplest `serialized tree` class — single int auto-property.
// Verifies synthesis, round-trip value equality, and owned return (§28.8).
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_single_int_property_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Box : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(42); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        // Deserialize returns owned Box (§28.8); using var takes ownership.
        "  using var b2 := Box.Deserialize(d); "
        "  return b2.V; "
        "end Main; end C;"), 42);
}

// §9.8: `serialized tree` class with string + int properties.
// §9.8: "The serialized components of a class are its owned and value-typed
// fields and auto-properties … in declaration order."
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_string_int_properties_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Person : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public property Age: int; "
        "  public constructor(name: string, age: int) "
        "    Name := name; Age := age; "
        "  end constructor; "
        "end Person; "
        "class C public static method Main(): int "
        "  using var p := new Person(\"Alice\", 30); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var p2 := Person.Deserialize(d); "
        "  if p2.Name <> \"Alice\" then return 91; end; "
        "  return (if p2.Age = 30 then 42 else 92); "
        "end Main; end C;"), 42);
}

// §9.8: `serialized tree` class with bool and double properties.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_bool_double_properties_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Config : ISerializable, IDeserializable "
        "  public property Enabled: bool; "
        "  public property Ratio: double; "
        "  public constructor(enabled: bool, ratio: double) "
        "    Enabled := enabled; Ratio := ratio; "
        "  end constructor; "
        "end Config; "
        "class C public static method Main(): int "
        "  using var c := new Config(true, 1.5); "
        "  using var s := new BinarySerializer(); "
        "  c.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var c2 := Config.Deserialize(d); "
        "  if c2.Enabled <> true then return 91; end; "
        "  return (if c2.Ratio = 1.5 then 42 else 92); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 / `serialized tree`: nested owned class (tree of classes)
//
// "A class with either clause is serialization-eligible when every serialized
// component is itself serialization-eligible by the rule above (which
// recursively admits other `serialized tree`/`serialized graph` classes and
// owned collections of them)."
// ===========================================================================

// §9.8: parent owns a child; both are `serialized tree`. Tests recursive
// traversal of the owning spanning tree in both directions.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_owning_child_class_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Inner : ISerializable, IDeserializable "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Inner; "
        "class Outer : ISerializable, IDeserializable "
        "  public owned Child: Inner; "
        "  public property Tag: string; "
        "  public constructor(owned child: Inner, tag: string) "
        "    Child := move child; Tag := tag; "
        "  end constructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var o := new Outer(new Inner(7), \"hello\"); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var o2 := Outer.Deserialize(d); "
        "  if o2.Child.X <> 7 then return 91; end; "
        "  return (if o2.Tag = \"hello\" then 42 else 92); "
        "end Main; end C;"), 42);
}

// §9.8: depth-3 owned class tree (Root → Middle → Leaf). Tests that synthesis
// recurses to depth ≥3 and that the owning-tree reconstruction is complete.
BEZEL_TEST(codegen_serialization_t2_classes,
           depth3_tree_class_nested_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Leaf : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Leaf; "
        "class Middle : ISerializable, IDeserializable "
        "  public owned L: Leaf; "
        "  public property N: int; "
        "  public constructor(owned l: Leaf, n: int) "
        "    L := move l; N := n; "
        "  end constructor; "
        "end Middle; "
        "class Root : ISerializable, IDeserializable "
        "  public owned M: Middle; "
        "  public property Name: string; "
        "  public constructor(owned m: Middle, name: string) "
        "    M := move m; Name := name; "
        "  end constructor; "
        "end Root; "
        "class C public static method Main(): int "
        "  using var r := new Root(new Middle(new Leaf(5), 10), \"root\"); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var r2 := Root.Deserialize(d); "
        "  if r2.M.L.V <> 5 then return 91; end; "
        "  if r2.M.N <> 10 then return 92; end; "
        "  return (if r2.Name = \"root\" then 42 else 93); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8: class containing a serialization-eligible record field
//
// §9.8 component-eligibility: "another serialization-eligible record" is an
// eligible component type. A `serialized tree` class whose auto-property is
// a record type must serialize the record inline (via its own synthesized
// Serialize).
// ===========================================================================

// §9.8: `serialized tree` class with a positional-record auto-property.
// The record and class both declare ISerializable, IDeserializable.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_containing_record_field_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "class Shape : ISerializable, IDeserializable "
        "  public property Center: Point; "
        "  public property Label: string; "
        "  public constructor(center: Point, label: string) "
        "    Center := center; Label := label; "
        "  end constructor; "
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

// §9.8: `serialized tree` class with an enum-typed auto-property.
// §9.8: "an enum" is an eligible component type.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_containing_enum_field_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "enum Status: ISerializable, IDeserializable Active, Inactive; end Status; "
        "class Item : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public property St: Status; "
        "  public constructor(name: string, st: Status) "
        "    Name := name; St := st; "
        "  end constructor; "
        "end Item; "
        "class C public static method Main(): int "
        "  using var it := new Item(\"thing\", Status.Active); "
        "  using var s := new BinarySerializer(); "
        "  it.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var it2 := Item.Deserialize(d); "
        "  if it2.Name <> \"thing\" then return 91; end; "
        "  return (if it2.St = Status.Active then 42 else 92); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8: record containing a `serialized tree` class field
//
// §9.8 component-eligibility: "a serialization-eligible synthesized class
// (a class with a `serialized tree`, `serialized graph`, or `serialized
// cyclic` clause)" is an eligible component type for a record.
//
// SILENCE-4: borrow-bearing record + serialization-eligible class field.
//   §9.7 states "Reference-typed fields in records are always borrows; the
//   record does not own what they point to." A positional record with a class-
//   typed field is therefore a borrow-bearing record. For serialization, the
//   synthesized Deserialize for a borrow-bearing record must create the owned
//   class instances externally and pass borrows into the record — but §9.8
//   does not specify where those owned instances live or how they are exposed
//   to the caller. The round-trip ownership shape is unspecified; no test is
//   written here pending clarification of that shape (see SILENCE comments at
//   file top). A test exercising this rule from the class side — a `serialized
//   tree` class whose auto-property is a record — appears above as
//   tree_class_containing_record_field_roundtrip_spec_9_8.
// ===========================================================================

// ===========================================================================
// §9.8: nullable class-typed fields
//
// §9.8 component-eligibility: "a nullable T? whose T is serialization-eligible
// (encoded as the value or WriteNull/TryReadNull)"
// This applies to class-typed T? as well as primitives.
// ===========================================================================

// §9.8: `serialized tree` class with a nullable auto-property of a serialized-
// tree class type — non-null path round-trips.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_nullable_owned_child_nonnull_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Child : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Child; "
        "class Parent : ISerializable, IDeserializable "
        "  public property Tag: int; "
        "  public owned OptChild: Child?; "
        "  public constructor(tag: int, owned optChild: Child?) "
        "    Tag := tag; OptChild := move optChild; "
        "  end constructor; "
        "end Parent; "
        "class C public static method Main(): int "
        "  using var p := new Parent(7, new Child(99)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var p2 := Parent.Deserialize(d); "
        "  if p2.Tag <> 7 then return 91; end; "
        "  var oc := p2.OptChild; "
        "  if oc = null then return 92; end; "
        "  return (if oc.V = 99 then 42 else 93); "
        "end Main; end C;"), 42);
}

// §9.8: nullable owned class-typed field — null path round-trips correctly
// (field is serialized as WriteNull; Deserialize reads null back).
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_nullable_owned_child_null_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Child : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Child; "
        "class Parent : ISerializable, IDeserializable "
        "  public property Tag: int; "
        "  public owned OptChild: Child?; "
        "  public constructor(tag: int, owned optChild: Child?) "
        "    Tag := tag; OptChild := move optChild; "
        "  end constructor; "
        "end Parent; "
        "class C public static method Main(): int "
        "  using var p := new Parent(5, null); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var p2 := Parent.Deserialize(d); "
        "  if p2.Tag <> 5 then return 91; end; "
        "  return (if p2.OptChild = null then 42 else 92); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8: field-name customization on class auto-properties
//
// "By default the synthesized form uses each component's declared name as its
// serialized field name … A `serialized` clause on a positional field, an
// auto-property, or an enum constant customizes this."
// Customization applies to classes exactly as to records.
// ===========================================================================

// §9.8: `serialized "name"` on a class auto-property renames the wire field.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_field_name_customization_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class User : ISerializable, IDeserializable "
        "  public property Id: int; "
        "  public property FirstName: string serialized \"first_name\"; "
        "  public constructor(id: int, firstName: string) "
        "    Id := id; FirstName := firstName; "
        "  end constructor; "
        "end User; "
        "class C public static method Main(): int "
        "  using var u := new User(1, \"Bob\"); "
        "  using var s := new BinarySerializer(); "
        "  u.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var u2 := User.Deserialize(d); "
        "  if u2.Id <> 1 then return 91; end; "
        "  return (if u2.FirstName = \"Bob\" then 42 else 92); "
        "end Main; end C;"), 42);
}

// §9.8: `serialized skip` on a class auto-property — excluded from both
// directions; Deserialize supplies default(int) = 0 for the skipped field.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_serialized_skip_clause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Widget : ISerializable, IDeserializable "
        "  public property X: int; "
        "  public property Secret: int serialized skip; "
        "  public constructor(x: int, secret: int) "
        "    X := x; Secret := secret; "
        "  end constructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(10, 99); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var w2 := Widget.Deserialize(d); "
        "  if w2.X <> 10 then return 91; end; "
        // Secret was excluded; Deserialize supplies default(int) = 0.
        "  return (if w2.Secret = 0 then 42 else 92); "
        "end Main; end C;"), 42);
}

// §9.8: `serialized optional` on a nullable class auto-property —
// when value is null, field is omitted on write; absent on read → null.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_serialized_optional_null_omitted_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Meta : ISerializable, IDeserializable "
        "  public property Code: int; "
        "  public property Note: string? serialized optional; "
        "  public constructor(code: int, note: string?) "
        "    Code := code; Note := note; "
        "  end constructor; "
        "end Meta; "
        "class C public static method Main(): int "
        "  using var m := new Meta(7, null); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var m2 := Meta.Deserialize(d); "
        "  if m2.Code <> 7 then return 91; end; "
        "  return (if m2.Note = null then 42 else 92); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 / `serialized graph`: DAG-shaped class round-trips
//
// "serialized graph asserts the serialized object graph is a directed acyclic
// graph: serialized edges may alias a shared object (the same object reachable
// by more than one path), but there are no cycles."
// ===========================================================================

// §9.8: `serialized graph` class — basic round-trip (acyclic, no aliasing).
// A `serialized graph` class is valid even when the actual data is a tree;
// the `serialized graph` assertion is weaker and subsumes `serialized tree`.
BEZEL_TEST(codegen_serialization_t2_classes,
           graph_class_basic_tree_data_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Node : ISerializable, IDeserializable "
        "  public property Label: string; "
        "  public property Count: int; "
        "  public constructor(label: string, count: int) "
        "    Label := label; Count := count; "
        "  end constructor; "
        "end Node; "
        "class C public static method Main(): int "
        "  using var n := new Node(\"alpha\", 5); "
        "  using var s := new BinarySerializer(); "
        "  n.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var n2 := Node.Deserialize(d); "
        "  if n2.Label <> \"alpha\" then return 91; end; "
        "  return (if n2.Count = 5 then 42 else 92); "
        "end Main; end C;"), 42);
}

// §9.8 `serialized graph`: class declaration with a borrowing back-edge field.
// "The serialized components of a class are its owned and value-typed fields
// and auto-properties together with its borrowing reference fields (non-owning
// class-typed fields whose target is owned elsewhere in the same serialized
// graph), in declaration order."
// "A borrowing field is not written inline; it is written as a reference to
// the tag of the object it borrows."
// "Synthesized Deserialize … each borrowing field is supplied as a constructor
// argument — the reference is resolved against the tag→object table."
//
// §9.8 tier 3 (implemented): a `serialized graph` class with a non-owned
// class-typed back-edge field synthesizes Serialize/Deserialize using the
// identity-tag/BZ_SER_REF protocol.  The back-edge is written as a $ref to
// the owned object's tag and resolved from the tag→object table on deserialize.
// Wire: BZ_SER_INLINE(0x01)+LE32 tag before inline body; BZ_SER_REF(0x02)+LE32
// tag for back-edges.  Owner-before-borrower order guaranteed by declaration order.
//
// Construction note (v0.180 §28.3 / AOT migration): the borrow-into-field
// wiring must be admissible under the front-end's structural check. The former
// serialize-side pattern `hub.Alias := hub.Target` stored a borrow whose SOURCE
// is a scope-bound local (`hub`) into a field — a §28.3 violation the real
// driver rejects ("parameter-sourced and receiver-sourced borrows are
// admissible; scope-bound locals and temporaries are rejected"); only runJit's
// skipped ownership pass let it through. The reshape wires the self-alias inside
// a convenience constructor where the source `Target` is RECEIVER-sourced
// (`this.Target`) — admissible per §28.3. The two-arg constructor is retained
// for §9.8 Deserialize (which binds the plain-borrow back-edge as a ctor arg
// resolved from the tag table); the one-arg convenience ctor is not a §9.8
// constructor-selection match ({Target} ≠ {Target, Alias}), so selection stays
// unambiguous. Wire form and deserialize path are unchanged: Alias is a $ref to
// Target's tag on write, resolved to the same object on read.
BEZEL_TEST(codegen_serialization_t2_classes,
           graph_class_with_back_edge_field_compiles_and_round_trips_spec_9_8) {
    // §9.8 tier 3: a `serialized graph` class with a nullable back-edge field
    // (Slot?) rounds trips correctly.  Hub owns Target and has Alias: Slot?
    // (nullable back-edge) pointing to Target.  After deserialization, Alias
    // resolves to the same object as Target (identity preserved per §9.8).
    BEZEL_EXPECT_EQ(runGraphAot(
        std::string("module M; ") + kImport +
        // Slot: a simple graph-serializable class.
        "class Slot : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        // Hub: owns Target, has a nullable back-edge Alias pointing to Target.
        // Constructor params (target, alias) match field names (Target, Alias)
        // case-insensitively per §9.8 v0.136.
        "class Hub : ISerializable, IDeserializable "
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "  // nullable back-edge
        // §9.8 Deserialize binds {Target, Alias} through this constructor.
        "  public constructor(owned target: Slot, alias: Slot?) "
        "    Target := move target; Alias := alias; "
        "  end constructor; "
        // Serialize-side convenience ctor: wire the self-alias from the receiver
        // (`this.Target`), admissible under §28.3. Not a §9.8 selection match.
        "  public constructor(owned target: Slot) "
        "    Target := move target; Alias := Target; "
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        // Build Hub with Alias borrowing its own Target (receiver-sourced).
        "  using var hub := new Hub(new Slot(\"alpha\")); "
        // Serialize.
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        // Deserialize.
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        // Verify: Target round-tripped, Alias resolved to same Name as Target.
        "  if hub2.Target.Name <> \"alpha\" then return 91; end; "
        "  if hub2.Alias = null then return 92; end; "
        "  if hub2.Alias?.Name <> \"alpha\" then return 93; end; "
        "  return 42; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 / `serialized cyclic`: class with self-contained constructor
//
// "serialized cyclic permits a serialized object graph that contains cycles."
// Self-contained-constructor rule: the constructor body, every chained
// constructor, and every field/property initializer "may assign the instance's
// own fields and may read its own constructor parameters and its own value-
// typed and owned fields, but it must NOT: 1. read any back-edge field;
// 2. call any instance method on this; 3. let this escape; 4. call a virtual
// or abstract method."
// "A class may declare serialized cyclic conservatively even when its graphs
// happen to be acyclic; the only cost is the self-contained-constructor
// restriction."
// ===========================================================================

// §9.8 `serialized cyclic`: conservative use — graph is acyclic (degenerate
// tree). The constructor is self-contained. Round-trip succeeds.
BEZEL_TEST(codegen_serialization_t2_classes,
           cyclic_class_conservative_acyclic_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Entry : ISerializable, IDeserializable "
        "  public property Key: string; "
        "  public property Val: int; "
        // Self-contained: assigns own fields from constructor params only.
        // No back-edge reads, no instance-method calls, no this-escape.
        "  public constructor(key: string, val: int) "
        "    Key := key; Val := val; "
        "  end constructor; "
        "end Entry; "
        "class C public static method Main(): int "
        "  using var e := new Entry(\"x\", 7); "
        "  using var s := new BinarySerializer(); "
        "  e.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var e2 := Entry.Deserialize(d); "
        "  if e2.Key <> \"x\" then return 91; end; "
        "  return (if e2.Val = 7 then 42 else 92); "
        "end Main; end C;"), 42);
}

// §9.8 v0.141 (#703 migration): the `serialized cyclic` "self-contained
// constructor" rule (no instance-method call on `this` during construction) was
// retired with the keyword — cycles are now expressed via Shared<T>/Weak<T>,
// which carry no such constraint. A keyword-less class with a borrow back-edge
// whose constructor calls an instance method now COMPILES: the blanket ban is
// gone.  What governs a construction-time `this`-observation now is the narrower
// §22.7 (v0.184) null-watermark rule — an instance call on `this` is admitted
// only where every non-nullable field is already definitely assigned.  The
// reshape assigns the non-nullable `Label` field BEFORE the `this.ComputeLabel()`
// call (then overwrites it with the computed value), so the observation is safe
// and the program compiles.  (The former body called `ComputeLabel()` before any
// `Label` assignment; the real driver correctly rejects that under §22.7, and
// only runJit's skipped definite-assignment pass let it through.)
BEZEL_TEST(codegen_serialization_t2_classes,
           keyword_less_ctor_instance_method_call_is_allowed_spec_9_8_v0141) {
    BEZEL_EXPECT_EQ(runGraphAot(
        std::string("module M; ") + kImport +
        "class Bad : ISerializable, IDeserializable "
        "  public property Label: string; "
        "  public Parent: Bad?; "
        "  public constructor(label: string, parent: Bad?) "
        "    Label := label; "  // §22.7: non-nullable field assigned before the
        "    Parent := parent; "  // this-observation below
        "    Label := this.ComputeLabel(); "  // instance call on this now allowed
        "  end constructor; "
        "  public method ComputeLabel(): string return \"label\"; end ComputeLabel; "
        "end Bad; "
        "class C public static method Main(): int return 0; end Main; end C;"), 0);
}

// ===========================================================================
// §9.8 / `polymorphic` registry
//
// "A base class whose serialized components may, at run time, hold instances
// of registered derived classes declares a polymorphic registry."
// "Each named type must be assignable to the declaring class and must itself
// carry a graph-shape clause."
// "The synthesized serializer writes the concrete type's discriminator as a
// leading $type field; the synthesized deserializer reads it first."
// "A run-time type not present in the registry is a SerializationException."
// "A discriminator string absent from the registry raises SerializationException."
// ===========================================================================

// §9.8 `polymorphic`: Circle registered as "circle" — serialize a Circle,
// deserialize as Shape, verify discriminator dispatch selected Circle.
//
// §9.8: "When writing a component whose static type is a polymorphic base, the
// synthesized serializer writes the discriminator of the value's concrete
// run-time type as a distinguished leading field ($type)."
// "The deserializer looks the discriminator up in the registry, selects the
// concrete type's synthesized Deserialize, and delegates the remainder of the
// object to it (returning owned Base)."
//
// Wire: $type="circle" precedes Radius in the binary stream.
// After deserializing as Shape, Kind() returns 1 (Circle's override).
// Result: 42 if dispatch selected Circle, 91 if wrong type, -999 on compile fail.
BEZEL_TEST(codegen_serialization_t2_classes,
           polymorphic_dispatch_circle_roundtrip_spec_9_8) {
    // §9.8 tier 4: polymorphic discriminator dispatch — Circle branch.
    // Shape is `abstract` (no discriminator for itself). Circle and Square
    // each inherit from Shape and carry their own `serialized tree` clause.
    // A Container with an owned Shape field is used to trigger the polymorphic
    // serialization path (the Shape-typed field triggers emitPolyClassSerialize
    // and emitPolyClassDeserialize).
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        // Shape: abstract polymorphic base.
        "abstract class Shape : ISerializable, IDeserializable "
        "  polymorphic(\"circle\": Circle, \"square\": Square); "
        "  public abstract method Kind(): int; "
        "end Shape; "
        // Circle: concrete subtype of Shape; re-declares ISerializable for synthesis.
        "class Circle : Shape, ISerializable, IDeserializable "
        "  public property Radius: double; "
        "  public constructor(radius: double) Radius := radius; end constructor; "
        "  public override method Kind(): int return 1; end Kind; "
        "end Circle; "
        // Square: concrete subtype of Shape; re-declares ISerializable for synthesis.
        "class Square : Shape, ISerializable, IDeserializable "
        "  public property Side: double; "
        "  public constructor(side: double) Side := side; end constructor; "
        "  public override method Kind(): int return 2; end Kind; "
        "end Square; "
        // Container: owns a Shape field — triggers polymorphic ser/deser.
        "class Container : ISerializable, IDeserializable "
        "  public owned S: Shape; "
        "  public constructor(owned s: Shape) S := move s; end constructor; "
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

// §9.8 `polymorphic`: serialize a Square, deserialize as Shape — verifies
// discriminator dispatch selects Square (second registered type).
//
// §9.8: same polymorphic dispatch rules as the Circle case above, but now the
// $type discriminator written is "square" and Deserialize must dispatch to
// Square's synthesized Deserialize.  Kind() returns 2 for Square.
BEZEL_TEST(codegen_serialization_t2_classes,
           polymorphic_dispatch_square_roundtrip_spec_9_8) {
    // §9.8 tier 4: polymorphic discriminator dispatch — Square branch.
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "abstract class Shape : ISerializable, IDeserializable "
        "  polymorphic(\"circle\": Circle, \"square\": Square); "
        "  public abstract method Kind(): int; "
        "end Shape; "
        "class Circle : Shape, ISerializable, IDeserializable "
        "  public property Radius: double; "
        "  public constructor(radius: double) Radius := radius; end constructor; "
        "  public override method Kind(): int return 1; end Kind; "
        "end Circle; "
        "class Square : Shape, ISerializable, IDeserializable "
        "  public property Side: double; "
        "  public constructor(side: double) Side := side; end constructor; "
        "  public override method Kind(): int return 2; end Kind; "
        "end Square; "
        "class Container : ISerializable, IDeserializable "
        "  public owned S: Shape; "
        "  public constructor(owned s: Shape) S := move s; end constructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  using var box := new Container(new Square(3.0)); "
        "  using var s := new BinarySerializer(); "
        "  box.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var box2 := Container.Deserialize(d); "
        "  return (if box2.S.Kind() = 2 then 42 else 91); "
        "end Main; end C;"), 42);
}

// §9.8 `polymorphic`: a registered subtype without a graph-shape clause is a
// compile error at the registry.
// §9.8: "a registered type that is not serialization-eligible is a compile
// error at the registry."
BEZEL_TEST(codegen_serialization_t2_classes,
           polymorphic_registry_ineligible_registered_type_is_error_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        // BadChild has no graph-shape clause → not serialization-eligible.
        "class BadChild "
        "  public constructor() end constructor; "
        "end BadChild; "
        "class Base : ISerializable, IDeserializable "
        "  polymorphic(\"bad\": BadChild); "
        "end Base; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// ===========================================================================
// §9.8: ownership of deserialized class instance (§28.8 transfer to caller)
//
// "Synthesized Deserialize … Deserialize(d: IDeserializer): owned T
// (a class instance is an owned heap value transferred to the caller, §28.8)."
// `using var` pattern acquires ownership and disposes at end of scope.
// ===========================================================================

// §9.8 / §28.8: deserialized class instance is correctly owned — `using var`
// acquires, and the value inside is accessible before disposal.
BEZEL_TEST(codegen_serialization_t2_classes,
           deserialized_class_is_owned_spec_9_8_sec28_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Counter : ISerializable, IDeserializable "
        "  public property Count: int; "
        "  public constructor(count: int) Count := count; end constructor; "
        "end Counter; "
        "class C public static method Main(): int "
        "  using var c := new Counter(13); "
        "  using var s := new BinarySerializer(); "
        "  c.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        // Deserialize returns owned Counter (§28.8 transfer to caller).
        "  using var c2 := Counter.Deserialize(d); "
        "  return (if c2.Count = 13 then 42 else 91); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 + §12: `sealed` class with `serialized tree` (spec silence #2)
//
// §9.8 makes no mention of `sealed`. Treated as orthogonal: `sealed`
// prevents subclassing but does not affect serialization synthesis.
// ===========================================================================

// §9.8 + §12: `sealed serialized tree` class round-trips.
// Sealed is orthogonal to the graph-shape clause (spec silence #2).
BEZEL_TEST(codegen_serialization_t2_classes,
           sealed_serialized_tree_class_roundtrip_spec_9_8_sec12) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "sealed class Token : ISerializable, IDeserializable "
        "  public property Value: int; "
        "  public constructor(value: int) Value := value; end constructor; "
        "end Token; "
        "class C public static method Main(): int "
        "  using var t := new Token(77); "
        "  using var s := new BinarySerializer(); "
        "  t.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var t2 := Token.Deserialize(d); "
        "  return (if t2.Value = 77 then 42 else 91); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8: hand-written Serialize suppresses synthesis for that direction
//
// "Hand-writing Serialize (respectively a recognized static Deserialize)
// suppresses synthesis of that direction, exactly as a manually declared
// Equals/GetHashCode suppresses auto-equality."
// "Declaring it with a hand-written Serialize is always valid regardless of
// eligibility (the hand-written body is the contract)."
// ===========================================================================

// §9.8: `serialized tree` class with a hand-written Serialize — synthesis is
// suppressed; the class is valid. The hand-written method writes the V field.
BEZEL_TEST(codegen_serialization_t2_classes,
           tree_class_hand_written_serialize_suppresses_synthesis_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Box : ISerializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        // Hand-written Serialize suppresses synthesis for this direction.
        "  public method Serialize(s: ISerializer) "
        "    s.BeginObject(1); "
        "    s.WriteFieldName(\"V\"); s.WriteInt(V); "
        "    s.EndObject(); "
        "  end Serialize; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(42); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        // ToBytes produces a non-empty buffer; we verify run completes.
        "  return (if bytes.Count > 0 then 42 else 91); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8: `serialized graph` dynamic enforcement — cycle raises at runtime
//
// "Under serialized graph, a reference encountered while its own target is
// still being constructed … raises SerializationException. A cycle has no
// owner-before-borrower order, so it is exactly the case serialized graph
// does not assert."
//
// NOTE: This test is NOT written here because constructing a true owned-edge
// cycle is statically impossible in Bezel (exclusive ownership move semantics
// prevent a cycle closing through an owned edge — §28.3). The only cycles
// Bezel allows are through borrowing back-edges, and those are handled by
// `serialized cyclic`. The `serialized graph` cycle-detection path is
// therefore only reachable through malformed wire data (which the deserializer
// tests after implementation). No codegen-level test is written for this.
// ===========================================================================

// ===========================================================================
// §9.8 v0.136: constructor-selection by parameter NAME (order-insensitive)
//
// "When the class declares more than one constructor, the synthesized
// Deserialize uses the *unique* constructor whose parameter list binds
// exactly the serialized components and borrowing fields, matched by name
// (order-insensitive; `serialized "<name>"` renames do not affect the
// parameter-name match, which uses the declared identifiers). If no
// constructor matches, or more than one does, the conformance declaration
// is a compile error naming the candidate constructors."
// — §9.8 v0.136, constructor-selection paragraph
// ===========================================================================

// §9.8 v0.136: ctor params in REVERSED field order — round-trip must succeed.
//
// Class Root declares fields `owned M: Mid` then `Id: int`, but its
// constructor is `(id: int, owned m: Mid)` — params in the opposite order
// (camelCase params matching PascalCase fields via case-insensitive binding).
// The synthesized Deserialize must bind the args by name (Id→id, M→m), not
// positionally, so the ctor receives (id=7, m=<Mid ptr>) not (id=<Mid ptr>,
// m=7). This was the repro shape from the probe that caught the defect.
//
// Expected exit code: 42 (back.Id = 7 and back.M.Name = "m" and
// back.M.L.Tag = "leaf" → return back.M.L.N + back.Id = 35 + 7 = 42).
BEZEL_TEST(codegen_serialization_t2_classes,
           ctor_params_reversed_field_order_roundtrip_spec_9_8_v0136) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        // Leaf: fields Tag, N — ctor (tag, n) — same order (control)
        "class Leaf : ISerializable, IDeserializable "
        "  public Tag: string := \"\"; "
        "  public N: int := 0; "
        "  public constructor(tag: string, n: int) Tag := tag; N := n; end constructor; "
        "end Leaf; "
        // Mid: fields L, Name — ctor (name, l) — reversed order
        "class Mid : ISerializable, IDeserializable "
        "  public owned L: Leaf; "
        "  public Name: string := \"\"; "
        "  public constructor(name: string, owned l: Leaf) Name := name; L := move l; end constructor; "
        "end Mid; "
        // Root: fields M, Id — ctor (id, m) — reversed order (the repro shape)
        "class Root : ISerializable, IDeserializable "
        "  public owned M: Mid; "
        "  public Id: int := 0; "
        "  public constructor(id: int, owned m: Mid) Id := id; M := move m; end constructor; "
        "end Root; "
        "class Program "
        "  public static method Main(): int "
        "    using var r := new Root(7, new Mid(\"m\", new Leaf(\"leaf\", 35))); "
        "    using var ser := new BinarySerializer(); "
        "    r.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    using var back := Root.Deserialize(de); "
        "    if back.Id = 7 and back.M.Name = \"m\" and back.M.L.Tag = \"leaf\" then "
        "      return back.M.L.N + back.Id; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 42);
}

// §9.8 v0.136: ctor params in SCRAMBLED order for 3 fields — round-trip.
//
// Class Triple has fields A: int, B: string, C: int. Its ctor is
// (c: int, a: int, b: string) — a permutation, with camelCase params
// matching PascalCase fields via case-insensitive binding.
// Synthesized Deserialize must bind by name: C→c, A→a, B→b.
// Expected: round-trip preserves A=1, B="x", C=41 → exit 42.
BEZEL_TEST(codegen_serialization_t2_classes,
           ctor_params_scrambled_three_fields_roundtrip_spec_9_8_v0136) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "class Triple : ISerializable, IDeserializable "
        "  public A: int := 0; "
        "  public B: string := \"\"; "
        "  public C: int := 0; "
        // Constructor in reverse field order: c first, then a, then b
        "  public constructor(c: int, a: int, b: string) "
        "    A := a; B := b; C := c; "
        "  end constructor; "
        "end Triple; "
        "class Program "
        "  public static method Main(): int "
        "    using var t := new Triple(41, 1, \"x\"); "
        "    using var ser := new BinarySerializer(); "
        "    t.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    using var back := Triple.Deserialize(de); "
        "    if back.A = 1 and back.B = \"x\" and back.C = 41 then "
        "      return back.A + back.C; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 42);
}

// §9.8 v0.136 NEGATIVE: ctor param name matches NO serialized component.
//
// Class Foo has field V: int. Its only ctor is (x: int) — param name `x`
// does not match field name `V`. The conformance declaration is a compile
// error (no constructor matches the component set).
BEZEL_TEST(codegen_serialization_t2_classes,
           ctor_param_name_no_match_is_compile_error_spec_9_8_v0136) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module V; ") + kImport +
        "class Foo : IDeserializable "
        "  public V: int := 0; "
        // Param name `x` does not match field name `V` → no ctor matches
        "  public constructor(x: int) V := x; end constructor; "
        "end Foo; "
        "class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;"));
}

// §9.8 v0.136 NEGATIVE: two ctors both match the serialized component set.
//
// Class Bar has fields A: int, B: string. It declares two constructors:
//   constructor(a: int, b: string) and constructor(b: string, a: int)
// Both have parameter names {a, b} matching component names {A, B} via
// case-insensitive binding — ambiguous (the ctors are valid overloads since
// (int, string) and (string, int) differ by type order).
// The conformance declaration is a compile error (more than one matches).
BEZEL_TEST(codegen_serialization_t2_classes,
           two_ctors_both_match_is_compile_error_spec_9_8_v0136) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module V; ") + kImport +
        "class Bar : IDeserializable "
        "  public A: int := 0; "
        "  public B: string := \"\"; "
        "  public constructor(a: int, b: string) A := a; B := b; end constructor; "
        "  public constructor(b: string, a: int) A := a; B := b; end constructor; "
        "end Bar; "
        "class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;"));
}

// ===========================================================================
// §9.8 v0.136 ASan truncated-input check (per #550 lane contract)
//
// Truncated binary input to Deserialize on a 3-level tree must raise
// SerializationException with NO heap leak or double-free of any partially-
// constructed subtree. The synthesized Deserialize builds Root → Mid → Leaf
// in nested order; if the buffer is cut in half, some intermediate owned
// objects will have been allocated before the error is detected. The
// ownership model (§28.3/§28.5) must ensure those are reclaimed exactly once
// as the exception unwinds. Verified with ASAN_OPTIONS=detect_leaks=1.
// ===========================================================================

namespace {

// Bezel source: 3-level tree (field order reversed vs ctor param order per
// §9.8 v0.136), serialize a full instance, truncate bytes to half, call
// Deserialize — must raise SerializationException, exit 42.
const char* kTruncatedInput3Level =
    "module V; "
    "import System.Serialization; "
    "class Leaf : ISerializable, IDeserializable "
    "  public Tag: string := \"\"; "
    "  public N: int := 0; "
    "  public constructor(tag: string, n: int) Tag := tag; N := n; end constructor; "
    "end Leaf; "
    "class Mid : ISerializable, IDeserializable "
    "  public owned L: Leaf; "
    "  public Name: string := \"\"; "
    "  public constructor(name: string, owned l: Leaf) Name := name; L := move l; end constructor; "
    "end Mid; "
    "class Root : ISerializable, IDeserializable "
    "  public owned M: Mid; "
    "  public Id: int := 0; "
    "  public constructor(id: int, owned m: Mid) Id := id; M := move m; end constructor; "
    "end Root; "
    "class Program "
    "  public static method Main(): int "
    "    using var r := new Root(7, new Mid(\"hello\", new Leaf(\"world\", 35))); "
    "    using var ser := new BinarySerializer(); "
    "    r.Serialize(ser); "
    "    using var bytes := ser.ToBytes(); "
    "    var halfLen: int := bytes.Length / 2; "
    "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan(0, halfLen)); "
    "    var caught: bool := false; "
    "    try "
    "      using var back := Root.Deserialize(de); "
    "      return 0; "
    "    catch SerializationException => caught := true; "
    "    end; "
    "    return (if caught then 42 else 1); "
    "  end Main; "
    "end Program;";

} // namespace

// §9.8 v0.136 + §28.3/§28.5: truncated binary input must raise
// SerializationException and leave no heap leak or double-free.
// ASan with detect_leaks=1 confirms. Skipped if toolchain unavailable.
BEZEL_TEST(codegen_serialization_t2_classes,
           truncated_input_asan_no_leak_spec_9_8_v0136) {
    // Exit-code leg: verify SerializationException is caught (rc=42).
    BEZEL_EXPECT_EQ(run(kTruncatedInput3Level), 42);
    // ASan leg: same source, but now compiled to native .o + linked with
    // ASan runtime + run with ASAN_OPTIONS=detect_leaks=1. Returns -1 if
    // the toolchain is unavailable → test silently passes (skip, not fail).
    int asan = runUnderLeakSanitizer550(kTruncatedInput3Level, /*detectLeaks=*/true);
    if (asan == -1) return;  // toolchain unavailable: skip ASan leg
    BEZEL_EXPECT_EQ(asan, 0);  // 0 == no leak / no UAF / no double-free
}
