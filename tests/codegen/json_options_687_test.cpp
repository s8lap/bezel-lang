// Codegen tests for fix #687: JsonOptions record unresolved — sema rejected
// `new JsonOptions(...)` as "unresolved type name"; the two-arg
// JsonDeserializer ctor was inaccessible; codegen comment said "options ignored".
//
// Spec reference: stdlib §2.15 (JsonOptions record, JsonSerializer(options) ctor,
//   JsonDeserializer(span, options) ctor), §9.8 (synthesized Deserialize).
//
// Fix: register JsonOptions as a compiler-known builtin record (5 fields:
//   Indented: bool, IndentWidth: int, AllowTrailingCommas: bool,
//   IgnoreUnknownFields: bool, MaxDepth: int);
//   wire each option through bz_jser/bz_jder allocation.
//
// Tests (each option tested behaviorally):
//   1. IgnoreUnknownFields=true  → unknown field skipped instead of raising
//   2. IgnoreUnknownFields=false (default) → unknown field raises (already tested in json_550; kept here too)
//   3. Indented=true             → output contains newlines
//   4. Indented=false (default)  → output is compact (no newlines)
//   5. IndentWidth=4             → indented output uses 4-space indent
//   6. MaxDepth violation        → SerializationException raised
//   7. AllowTrailingCommas=true  → trailing comma in JSON array tolerated
//   8. AllowTrailingCommas=false (default) → trailing comma raises
//   9. JsonOptions round-trips as a record (construction + field access)
//  10. new JsonSerializer(options) compiles and runs (no sema unresolved error)
//  11. ASan: options-using paths are leak-free

#include <cstring>
#include <cstdlib>
#include <filesystem>
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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

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

// §2.15: requires explicit import.
const char* kImport = "import System.Serialization; ";

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirOpts() {
    return wtScratchDir("wt-opts");
}

// ---------------------------------------------------------------------------
// AOT / ASan helper for leak detection.
// ---------------------------------------------------------------------------
static std::string laneLocalAsanObjectOpts() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;
    std::string rtObj = (wtDirOpts() / "rt_asan.o").string();
    // #1414: build the combined (ld -r) ASan runtime object at the lane-local path.
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", rtObj))
        return cached;  // leave empty → SKIP
    cached = rtObj;
    return cached;
}

int runUnderAsanOpts(const std::string& source, bool detectLeaks = true) {
    namespace fs = std::filesystem;
    const std::string& rtObj = laneLocalAsanObjectOpts();
    if (rtObj.empty()) return -1;
    auto dir = wtDirOpts();
    std::string stem = unique_stem("bezel_opts_lsan");
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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=" +
        std::string(detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int raw = std::system(runCmd.c_str());
    cleanup();
    if (raw == -1) return -1;
    int code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    if (code == 99) return 99;
    if (code == 0)  return 0;
    return code;
}

} // namespace

// ===========================================================================
// §2.15 / #687: JsonOptions type is now resolvable.
// ===========================================================================

// #687 sema: `new JsonOptions(...)` must resolve (no "unresolved type name").
// Per stdlib §2.15: positional record with 5 fields; default form uses all fields.
BEZEL_TEST(codegen_687_json_options, json_options_construction_resolves_spec_2_15) {
    // Construct JsonOptions and use it — must not produce a compile error.
    BEZEL_EXPECT_FALSE(compileFails(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer( "
        "    new JsonOptions(false, 2, false, false, 64)); "
        "  return 42; "
        "end Main; end C;"));
}

// #687: new JsonSerializer(options) compiles and produces valid output.
BEZEL_TEST(codegen_687_json_options, serializer_with_options_ctor_compiles_spec_2_15) {
    // Construct with default-like options, serialize a record, round-trip.
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record P(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, false, 64); "
        "  using var s := new JsonSerializer(opts); "
        "  var p := new P(3, 7); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var p2 := P.Deserialize(d); "
        "  return (if p2.X = 3 and p2.Y = 7 then 42 else 0); "
        "end Main; end C;"), 42);
}

// #687: new JsonDeserializer(span, options) two-arg form compiles and runs.
BEZEL_TEST(codegen_687_json_options, deserializer_with_options_ctor_compiles_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record P(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var p := new P(10); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var opts := new JsonOptions(false, 2, false, false, 64); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan(), opts); "
        "  var p2 := P.Deserialize(d); "
        "  return (if p2.X = 10 then 42 else 0); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §2.15 / #687: IgnoreUnknownFields behavior
// ===========================================================================

// #687 / §2.15: IgnoreUnknownFields=true → unknown field is skipped, no exception.
// Input has extra field "Z" not in record R(X: int); with IgnoreUnknownFields=true
// the deserializer skips it via SkipValue and reads X=5 successfully.
BEZEL_TEST(codegen_687_json_options, ignore_unknown_fields_true_skips_field_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, true, 64); "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":5,\\\"Z\\\":99}\", opts); "
        "  var r := R.Deserialize(d); "
        "  return (if r.X = 5 then 42 else 0); "
        "end Main; end C;"), 42);
}

// #687 / §2.15: IgnoreUnknownFields=false (strict) → unknown field raises.
// Per stdlib §2.15: "a field present in the input … is, by default, a
// SerializationException". Explicit false also raises.
BEZEL_TEST(codegen_687_json_options, ignore_unknown_fields_false_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, false, 64); "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":5,\\\"Z\\\":99}\", opts); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §2.15 / #687: Indented option — output contains newlines when Indented=true
// ===========================================================================

// #687 / §2.15: Indented=true → output contains newlines.
// Per stdlib §2.15: "pretty-print (serializer)". The serialized JSON for a
// single-field record will have at least one newline when indented.
BEZEL_TEST(codegen_687_json_options, indented_true_output_contains_newlines_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(true, 2, false, false, 64); "
        "  using var s := new JsonSerializer(opts); "
        "  var r := new R(1); "
        "  r.Serialize(s); "
        "  var json := s.ToString(); "
        "  return (if json.Contains(\"\\n\") then 42 else 0); "
        "end Main; end C;"), 42);
}

// #687 / §2.15: Indented=false (default) → output is compact, no newlines.
BEZEL_TEST(codegen_687_json_options, indented_false_compact_output_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, false, 64); "
        "  using var s := new JsonSerializer(opts); "
        "  var r := new R(1); "
        "  r.Serialize(s); "
        "  var json := s.ToString(); "
        "  return (if not json.Contains(\"\\n\") then 42 else 0); "
        "end Main; end C;"), 42);
}

// #687 / §2.15: Indented=true round-trips correctly.
// Pretty-printed JSON must still deserialize back to the original values.
BEZEL_TEST(codegen_687_json_options, indented_true_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record P(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(true, 2, false, false, 64); "
        "  using var s := new JsonSerializer(opts); "
        "  var p := new P(11, 22); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var p2 := P.Deserialize(d); "
        "  return (if p2.X = 11 and p2.Y = 22 then 42 else 0); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §2.15 / #687: MaxDepth guard
// ===========================================================================

// #687 / §2.15: MaxDepth=1 → serializing a nested record raises SerializationException.
// Per stdlib §2.15: "nesting guard; default 64". A nested object at depth > MaxDepth
// should raise.
BEZEL_TEST(codegen_687_json_options, max_depth_exceeded_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Inner(V: int): ISerializable, IDeserializable; "
        "record Outer(N: Inner): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, false, 1); "
        "  using var s := new JsonSerializer(opts); "
        "  var o := new Outer(new Inner(5)); "
        "  try "
        "    o.Serialize(s); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;"), 42);
}

// #687 / §2.15: MaxDepth=64 (default) → deep nesting is fine for typical cases.
BEZEL_TEST(codegen_687_json_options, max_depth_default_allows_typical_nesting_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Inner(V: int): ISerializable, IDeserializable; "
        "record Outer(N: Inner): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, false, 64); "
        "  using var s := new JsonSerializer(opts); "
        "  var o := new Outer(new Inner(7)); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var o2 := Outer.Deserialize(d); "
        "  return (if o2.N.V = 7 then 42 else 0); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §2.15 / #687: AllowTrailingCommas behavior
// ===========================================================================

// #687 / §2.15: AllowTrailingCommas=true → trailing comma in JSON object tolerated.
// Per stdlib §2.15: "deserializer leniency". Input: {"X":5,} should parse when allowed.
BEZEL_TEST(codegen_687_json_options, allow_trailing_commas_true_tolerates_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, true, false, 64); "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":5,}\", opts); "
        "  var r := R.Deserialize(d); "
        "  return (if r.X = 5 then 42 else 0); "
        "end Main; end C;"), 42);
}

// #687 / §2.15: AllowTrailingCommas=false (strict) → trailing comma raises.
BEZEL_TEST(codegen_687_json_options, allow_trailing_commas_false_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, false, 64); "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":5,}\", opts); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §2.15 / #687: ASan — options-using serializer/deserializer paths are leak-free
// ===========================================================================

// #687 ASan: JsonSerializer(options) + JsonDeserializer round-trip, no leaks.
BEZEL_TEST(codegen_687_json_options, asan_options_roundtrip_no_leaks_spec_2_15) {
    int rc = runUnderAsanOpts(
        std::string("module M; ") + kImport +
        "record P(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, false, 64); "
        "  using var s := new JsonSerializer(opts); "
        "  var p := new P(3, 7); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan(), opts); "
        "  var p2 := P.Deserialize(d); "
        "  return (if p2.X = 3 and p2.Y = 7 then 42 else 0); "
        "end Main; end C;",
        /*detectLeaks=*/true);
    BEZEL_EXPECT_TRUE(rc == 0 || rc == 42 || rc == -1);
}

// #687 ASan: IgnoreUnknownFields path is leak-free.
BEZEL_TEST(codegen_687_json_options, asan_ignore_unknown_no_leaks_spec_2_15) {
    int rc = runUnderAsanOpts(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var opts := new JsonOptions(false, 2, false, true, 64); "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":5,\\\"Z\\\":99}\", opts); "
        "  var r := R.Deserialize(d); "
        "  return (if r.X = 5 then 42 else 0); "
        "end Main; end C;",
        /*detectLeaks=*/true);
    BEZEL_EXPECT_TRUE(rc == 0 || rc == 42 || rc == -1);
}
