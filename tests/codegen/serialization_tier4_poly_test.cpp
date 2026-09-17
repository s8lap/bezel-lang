// Spec-derived codegen tests — §9.8 serialization synthesis, TIER 4:
// `polymorphic` registry dispatch.
// Issue #550 tier 4 (feat-550-t4-registries).
//
// Tests in this file verify the tier-4 implementation of the §9.8 polymorphic
// registry:
//   - Binary round-trip (Circle dispatched via "circle", Square via "square")
//   - JSON round-trip (same hierarchy, JsonSerializer/JsonDeserializer)
//   - Unregistered run-time type → SerializationException on Serialize
//   - Unknown discriminator string → SerializationException on Deserialize
//   - Polymorphic × graph back-ref composition ($id/$type on first occurrence,
//     pure $ref on back-references)
//   - Registry compile errors: duplicate discriminator, ineligible type,
//     registered type not a subtype of the base
//
// All APIs verified against Bezel_Standard_Library.md.
// No invented APIs.
//
// Wire-format note (§9.8 + §2.15 v0.139):
//   The discriminator field name is "$type" (reserved in the §9.8 prose;
//   "$" prefix reserved per §2.15 v0.139).  For binary: it is written as an
//   ordinary field (field-name string + value string) before all component
//   fields.  For JSON: the leading field in the object is "$type":"<disc>".
//
// Spec refs: §9.8 (v0.139), §2.14, §2.15, §28.8.

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

// §9.8 polymorphic × graph back-edge round-trip migrated to the AOT harness
// (real driver, full front-end incl. §28.3 ownership). runJit masked the former
// serialize-side borrow-into-field wiring by skipping the ownership pass; the AOT
// path is the correct oracle. See the migrated test's note on the reshape.
int runGraphAot(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool compileFails(const std::string& src) {
    auto r = runJit(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

// Run `source` under ASan detect_leaks=1.  Returns 0 (clean), 1 (sanitizer
// triggered), or -1 (toolchain unavailable/skip).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    namespace fs = std::filesystem;
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_t4_lsan_" + std::to_string(counter++);
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

// Shared hierarchy used by several tests:
//
//   Shape — abstract polymorphic base (`: ISerializable` with `polymorphic` member clause)
//   Circle : Shape — Radius: double, Kind() → 1
//   Square : Shape — Side: double,   Kind() → 2
//
// Shape is `abstract` (cannot be instantiated; no constructor on Shape itself).
// Shape.Kind() is abstract; each concrete subtype overrides it.
// This lets us verify which concrete type was deserialized without a cast.
//
// §9.8: "each named type must be assignable to the declaring class" — Circle
// and Square both inherit from Shape, satisfying this requirement.
// §9.8: "each registered type must itself carry a serialization eligibility
// clause" — both Circle and Square carry `: ISerializable, IDeserializable`.
// §9.8: "The declaring base class may be abstract (it registers none for
// itself; an input discriminator naming the base is then a SerializationException)."
//
// Circle and Square explicitly re-declare ISerializable so that the sema
// can identify them as serialization-eligible and synthesize their Serialize.
// (ISerializable is inherited from Shape, but explicit re-declaration is
// needed for synthesis recognition in the current implementation.)
const char* kHierarchy =
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
    "end Square; ";

} // namespace

// ===========================================================================
// §9.8 tier 4: binary format round-trips
// ===========================================================================

// Helper: a Container that owns a Shape field (polymorphic).
// Container's synthesized Serialize dispatches via emitPolyClassSerialize
// for the owned Shape field S; synthesized Deserialize reads $type via
// emitPolyClassDeserialize and binds the result to S.
// This is the canonical round-trip pattern for a polymorphic-base field.
//
// Note: Shape is abstract, so Container.S must be passed as an owned Circle
// or Square argument to the constructor.
const char* kContainer =
    "class Container : ISerializable, IDeserializable "
    "  public owned S: Shape; "
    "  public constructor(owned s: Shape) S := move s; end constructor; "
    "end Container; ";

// §9.8 tier 4: Circle binary round-trip through polymorphic base field.
// Container.S (Shape-typed) triggers emitPolyClassSerialize → writes
// $type="circle" + Radius:5.0.  On deserialize emitPolyClassDeserialize
// reads $type, dispatches to Circle's synthesized Deserialize.
// Container.S.Kind() == 1 confirms Circle was reconstructed.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_circle_binary_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kHierarchy + kContainer +
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

// §9.8 tier 4: Square binary round-trip through polymorphic base field.
// Wire: $type="square" + Side:3.0.  Dispatch selects Square; Kind() == 2.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_square_binary_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kHierarchy + kContainer +
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

// ===========================================================================
// §9.8 tier 4: JSON format round-trips
// ===========================================================================

// §9.8 + §2.15: Circle JSON round-trip through polymorphic base field.
// JSON wire for the Container.S field: {"$type":"circle","Radius":5.0}
// The "$type" field is the leading field per §9.8.
// Dispatch selects Circle; Kind() == 1.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_circle_json_roundtrip_spec_9_8_sec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kHierarchy + kContainer +
        "class C public static method Main(): int "
        "  using var box := new Container(new Circle(5.0)); "
        "  using var s := new JsonSerializer(); "
        "  box.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var box2 := Container.Deserialize(d); "
        "  return (if box2.S.Kind() = 1 then 42 else 91); "
        "end Main; end C;"), 42);
}

// §9.8 + §2.15: Square JSON round-trip through polymorphic base field.
// JSON wire for the Container.S field: {"$type":"square","Side":3.0}
// Dispatch selects Square; Kind() == 2.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_square_json_roundtrip_spec_9_8_sec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kHierarchy + kContainer +
        "class C public static method Main(): int "
        "  using var box := new Container(new Square(3.0)); "
        "  using var s := new JsonSerializer(); "
        "  box.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var box2 := Container.Deserialize(d); "
        "  return (if box2.S.Kind() = 2 then 42 else 91); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 tier 4: field-data round-trip (verify concrete fields are preserved)
// ===========================================================================

// §9.8 tier 4: Circle's Radius field is round-tripped correctly through the
// polymorphic path.  Uses a Container that owns a Circle as a Shape-typed
// owned field, so the entire container ser/deser path exercises both field-
// level poly dispatch (emitSerializeField / emitDeserializeField) and the
// virtual-method call on the reconstructed instance.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_circle_field_data_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kHierarchy +
        // Container owns a Shape field (polymorphic — concrete is Circle or Square).
        "class Container : ISerializable, IDeserializable "
        "  public owned S: Shape; "
        "  public Tag: int; "
        "  public constructor(owned s: Shape, tag: int) "
        "    S := move s; Tag := tag; "
        "  end constructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  using var box := new Container(new Circle(7.5), 99); "
        "  using var s := new BinarySerializer(); "
        "  box.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var box2 := Container.Deserialize(d); "
        // Verify Tag round-tripped and the owned Shape is a Circle.
        "  if box2.Tag <> 99 then return 91; end; "
        "  return (if box2.S.Kind() = 1 then 42 else 92); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 tier 4: failure paths — SerializationException
// ===========================================================================

// §9.8: "A run-time type not present in the registry is a SerializationException
// at serialization — the closed set is enforced on both directions."
//
// Rogue extends Shape but is NOT registered in Shape's polymorphic registry
// (only Circle and Square are).  When a Shape-typed field holds a Rogue at
// runtime, the emitPolyClassSerialize vtable-lookup finds no matching
// discriminator → raises SerializationException (caught, exit 42).
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_unregistered_runtime_type_raises_on_serialize_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kHierarchy + kContainer +
        // Rogue extends Shape but is not in the registry.
        "class Rogue : Shape, ISerializable, IDeserializable "
        "  public constructor() end constructor; "
        "  public override method Kind(): int return 99; end Kind; "
        "end Rogue; "
        "class C public static method Main(): int "
        // Container.S is Shape-typed; holds Rogue at runtime.
        // When box.Serialize(s) serializes the S field through emitPolyClassSerialize,
        // it checks the vtable of S → Rogue is not in the registry → exception.
        "  using var box := new Container(new Rogue()); "
        "  using var s := new BinarySerializer(); "
        "  var caught: bool := false; "
        "  try "
        "    box.Serialize(s); "
        "    return 0; "  // reached only if no exception — wrong
        "  catch SerializationException => caught := true; "
        "  end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

// §9.8: "A discriminator string absent from the registry raises
// SerializationException."
//
// We construct a binary buffer manually (by serializing a Circle and then
// byte-patching the discriminator string to "triangle" — an unknown value).
// Calling Shape.Deserialize on the patched buffer must raise
// SerializationException (caught, exit 42).
//
// Implementation note: rather than byte-patching (which is format-internal),
// we write a raw BinarySerializer byte sequence containing the "triangle"
// discriminator directly.  The BinarySerializer bytes for a single
// field-name/"$type"/value/"triangle" prefix followed by a 0-field remainder
// can be injected by serializing a hand-crafted inner type.
//
// Simpler approach that avoids knowledge of the wire format: use a parallel
// "Fake" hierarchy that writes the same object header structure but with an
// unregistered discriminator.  We compile a FakeShape whose Serialize writes
// $type="triangle".  We then feed those bytes to Shape.Deserialize.
//
// Actually the cleanest portable approach: Serialize a Circle (known good
// bytes), then deserialize with a DIFFERENT BinaryDeserializer fed a
// deliberately-bad in-memory buffer that has just enough framing to look like
// a valid binary object whose $type="triangle".  This is fragile against
// format changes.
//
// Instead: test the discriminator-absent failure by using JSON, which lets us
// write the JSON string directly.  The JSON bytes are human-readable:
// {"$type":"triangle"} — we build that string, convert to bytes, feed to
// JsonDeserializer, and call Shape.Deserialize.
//
// §9.8: failure is not format-specific; the discriminator lookup happens after
// the "$type" field is read regardless of format.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_unknown_discriminator_raises_on_deserialize_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kHierarchy +
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        // Write a JSON object with an unknown $type discriminator "triangle".
        // BeginObject fieldCount is advisory for JSON (ignored by JsonSerializer);
        // we use 1 field: $type = "triangle".
        "  s.BeginObject(1); "
        "  s.WriteFieldName(\"$type\"); s.WriteString(\"triangle\"); "
        "  s.EndObject(); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Shape.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; "
        "  end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 tier 4: polymorphic × graph back-ref composition
//
// §9.8: "When a polymorphic object is also a shared or cyclic reference target
// ... the object's first occurrence carries its identity tag and, because it is
// polymorphic, its $type discriminator, then its body; every subsequent
// occurrence is a pure $ref to that identity tag and carries no $type and no
// body."
// ===========================================================================

// §9.8 × §9.8 back-ref: a graph-shape Container holds an owned Shape
// (polymorphic) and a borrow to the same Shape (back-edge).  Serialize writes
// the Shape's $type + $id on first occurrence; the back-edge writes only $ref.
// Deserialize reconstructs the Container with both fields pointing to the
// same concrete (Circle) instance.
// Construction (v0.180 §28.3 / AOT migration): the self-alias is wired inside a
// convenience constructor from the receiver (`this.Primary` — receiver-sourced,
// admissible per §28.3), replacing the former `hub.Alias := hub.Primary` which
// stored a borrow sourced from the scope-bound local `hub` into a field (rejected
// by the real driver; only runJit's skipped ownership pass accepted it). The
// two-arg ctor is retained for §9.8 Deserialize; the one-arg ctor is not a §9.8
// selection match, so selection stays unambiguous.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_graph_backref_composition_spec_9_8) {
    BEZEL_EXPECT_EQ(runGraphAot(
        std::string("module M; ") + kImport + kHierarchy +
        // Hub: owns a Shape (polymorphic) and borrows the same via Alias.
        "class Hub : ISerializable, IDeserializable "
        "  public owned Primary: Shape; "
        "  public Alias: Shape?; "  // nullable back-edge
        "  public constructor(owned primary: Shape, alias: Shape?) "
        "    Primary := move primary; Alias := alias; "
        "  end constructor; "
        // Serialize-side convenience ctor: wire the self-alias from the receiver.
        "  public constructor(owned primary: Shape) "
        "    Primary := move primary; Alias := Primary; "
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        "  using var hub := new Hub(new Circle(4.0)); "
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        // Primary dispatched to Circle via $type; Alias resolved from $ref.
        "  if hub2.Primary.Kind() <> 1 then return 91; end; "
        "  if hub2.Alias = null then return 92; end; "
        "  return (if hub2.Alias?.Kind() = 1 then 42 else 93); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 tier 4: ASan no-leak on poly round-trips
// ===========================================================================

// §9.8 tier 4 + §28.3/§28.5: polymorphic round-trip (Circle) under ASan
// detect_leaks=1 must leave no heap leak.  The `using var` disposal of
// box2 (a Container owning a Shape/Circle) must call Circle's Dispose.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_circle_binary_roundtrip_asan_no_leak_spec_9_8) {
    const std::string src =
        std::string("module M; ") + kImport + kHierarchy + kContainer +
        "class Program public static method Main(): int "
        "  using var box := new Container(new Circle(5.0)); "
        "  using var s := new BinarySerializer(); "
        "  box.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var box2 := Container.Deserialize(d); "
        "  return (if box2.S.Kind() = 1 then 42 else 91); "
        "end Main; end Program;";
    // JIT leg: verify correct exit code.
    BEZEL_EXPECT_EQ(run(src), 42);
    // ASan leg: detect_leaks=1 — verify no heap leak from the deserialized Circle.
    int asan = runUnderLeakSanitizer(src, /*detectLeaks=*/true);
    if (asan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(asan, 0);
}

// §9.8 tier 4 + §28.3/§28.5: failure path (unregistered type) under ASan.
// The SerializationException must not leak the BinarySerializer buffer or
// any partial state.  The exception is caught; exit code = 42 (clean).
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_unregistered_type_failure_asan_no_leak_spec_9_8) {
    const std::string src =
        std::string("module M; ") + kImport + kHierarchy + kContainer +
        // Rogue: in registry? No — only Circle and Square are registered.
        "class Rogue : Shape, ISerializable, IDeserializable "
        "  public constructor() end constructor; "
        "  public override method Kind(): int return 99; end Kind; "
        "end Rogue; "
        "class Program public static method Main(): int "
        "  using var box := new Container(new Rogue()); "
        "  using var s := new BinarySerializer(); "
        "  var caught: bool := false; "
        "  try "
        "    box.Serialize(s); "
        "    return 0; "
        "  catch SerializationException => caught := true; "
        "  end; "
        "  return (if caught then 42 else 1); "
        "end Main; end Program;";
    BEZEL_EXPECT_EQ(run(src), 42);
    int asan = runUnderLeakSanitizer(src, /*detectLeaks=*/true);
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// ===========================================================================
// §9.8 tier 4: compile-error tests for registry violations
// ===========================================================================

// §9.8: "Each registry entry's discriminator must be a non-empty string
// literal, unique within the clause."
// Two entries with the same discriminator → compile error at the registry.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_duplicate_discriminator_is_error_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        // Parent: abstract polymorphic base (member-clause form).
        "abstract class Parent : ISerializable, IDeserializable "
        "  polymorphic(\"dup\": Child, \"dup\": Child); "
        "end Parent; "
        // Child: subtype of Parent.
        "class Child : Parent, ISerializable, IDeserializable "
        "  public constructor() end constructor; "
        "end Child; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// §9.8: "each named type must be assignable to the declaring class"
// A registered type that does NOT inherit from the base → compile error.
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_non_subtype_registered_type_is_error_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        // Unrelated: serialization-eligible but does NOT extend Base.
        "class Unrelated : ISerializable, IDeserializable "
        "  public constructor() end constructor; "
        "end Unrelated; "
        "class Base : ISerializable, IDeserializable "
        "  polymorphic(\"x\": Unrelated); "
        "end Base; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// §9.8: "a registered type that is not serialization-eligible is a compile
// error at the registry."
// A registered type with no graph-shape clause → compile error.
// (This is a re-statement of the existing tier-boundary test, now with
// inheritance properly in place — BadChild extends Base but has no
// `serialized` clause, so it fails the eligibility check.)
BEZEL_TEST(codegen_serialization_t4_poly,
           poly_ineligible_registered_type_is_error_spec_9_8) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        // BadChild inherits from Base but carries no ISerializable/IDeserializable.
        "class BadChild : Base "
        "  public constructor() end constructor; "
        "end BadChild; "
        "class Base : ISerializable, IDeserializable "
        "  polymorphic(\"bad\": BadChild); "
        "end Base; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}
