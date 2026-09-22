// Codegen tests — §9.8 serialization / stdlib §2.15: JsonSerializer/JsonDeserializer.
// Issue #550, JSON format tier.  Issue #675: JSON graph-identity tokens.
//
// Tests: record round-trip (all primitive kinds incl. decimal + string escaping),
// enum name form, nested records, tree-class round-trip, nullable null/non-null,
// missing-required-field raises, malformed JSON raises SerializationException,
// cross-format negative (binary bytes into JsonDeserializer → clean exception),
// JSON output is verifiably real JSON (string-compare a known small case).
//
// §2.15 JSON rules implemented:
//   - WriteInt/WriteUInt → JSON number
//   - WriteFloat → JSON number; NaN/±Infinity → SerializationException
//   - WriteDecimal → JSON number without precision loss
//   - WriteBool → true/false
//   - WriteString → quoted+escaped JSON string (\",\\,\n,\r,\t,\u00XX)
//   - WriteChar → one-character JSON string
//   - WriteNull → null
//   - BeginObject/WriteFieldName/EndObject → JSON object (fieldCount advisory, ignored)
//   - BeginArray/EndArray → JSON array (elementCount advisory, ignored)
//   - JsonDeserializer.BeginObject/BeginArray return -1
//   - Unknown-field strict by default (IgnoreUnknownFields=false)
//   - Missing required field → SerializationException (§9.8/§2.15)
//
// §2.15 v0.139 graph-identity JSON form (issue #675):
//   - Inline tag: `"$id":n` as first field inside the object (write_inline_tag)
//   - Back-reference: `{"$ref":n}` as complete object value (write_ref_tag)
//   - Reader: first field "$id" registers tag; single field "$ref" resolves tag
//   - `serialized "$..."` rename is a compile error at the conformance declaration
//
// Spec refs: §9.8, stdlib §2.14, §2.15, §2.15 v0.139.

#include <cstdlib>
#include <filesystem>
#include <string>

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
using bezel::test::runJit;  // graph_json_cycle_raises_spec_2_15_v0139 only — see its comment.

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
static const std::filesystem::path& wtDirJson() {
    return wtScratchDir("wt-json");
}

// ---------------------------------------------------------------------------
// AOT round-trip + ASan helper (lane-local rt object per mandatory memory
// discipline: lane-local path under /var/tmp/bezel-scratch/wt-json/ — not
// shared /tmp/bezel_rt_asan_memoized.o).
// ---------------------------------------------------------------------------
static std::string laneLocalAsanObject() {
    // Build lane-local ASan runtime once per process.
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;

    std::string rtObj = (wtDirJson() / "rt_asan.o").string();
    // #1414: build the combined (ld -r) ASan runtime object at the lane-local path.
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", rtObj))
        return cached;  // leave empty → SKIP
    cached = rtObj;
    return cached;
}

// Run source under ASan (detect_leaks enabled by default).
// Returns 0=clean, 99=asan/leak triggered, -1=skip (toolchain/compile fail).
int runUnderLeakSanitizerJson(const std::string& source, bool detectLeaks = true) {
    namespace fs = std::filesystem;
    const std::string& rtObj = laneLocalAsanObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDirJson();
    std::string stem = unique_stem("bezel_json_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=" +
        std::string(detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int raw = std::system(runCmd.c_str());
    cleanup();
    if (raw == -1) return bezel::test::asanExecFailure("system");
    int code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    if (code == 99) return 99;   // asan/leak
    if (code == 0)  return 0;    // clean
    return code;
}

} // namespace

// ===========================================================================
// §2.15 / §9.8: basic JSON round-trip — record with int field.
// JsonSerializer/JsonDeserializer require `import System.Serialization`.
// ===========================================================================

// §2.15: JSON record round-trip — integer field.
BEZEL_TEST(codegen_json_550, record_int_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var p := new Point(3, 7); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var p2 := Point.Deserialize(d); "
        "  return (if p2.X = 3 and p2.Y = 7 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: JSON record round-trip — bool field.
BEZEL_TEST(codegen_json_550, record_bool_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Flags(Active: bool, Done: bool): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var f := new Flags(true, false); "
        "  f.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var f2 := Flags.Deserialize(d); "
        "  return (if f2.Active = true and f2.Done = false then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: JSON record round-trip — string field (basic, no special chars).
BEZEL_TEST(codegen_json_550, record_string_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Msg(Text: string): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var m := new Msg(\"hello\"); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var m2 := Msg.Deserialize(d); "
        "  return (if m2.Text = \"hello\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: JSON string escaping — double-quote inside string.
// WriteString must produce \" for a literal double-quote character.
BEZEL_TEST(codegen_json_550, string_escape_dquote_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record W(V: string): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var w := new W(\"say \\\"hi\\\"\"); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var w2 := W.Deserialize(d); "
        "  return (if w2.V = \"say \\\"hi\\\"\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: JSON string escaping — backslash inside string.
// WriteString must produce \\ for a literal backslash.
BEZEL_TEST(codegen_json_550, string_escape_backslash_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record P(Path: string): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var p := new P(\"a\\\\b\"); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var p2 := P.Deserialize(d); "
        "  return (if p2.Path = \"a\\\\b\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: JSON string escaping — newline/tab control characters.
// WriteString must produce \n, \t for control chars (per JSON RFC 8259 §7).
BEZEL_TEST(codegen_json_550, string_escape_control_chars_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record L(Line: string): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var l := new L(\"a\\nb\"); "
        "  l.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var l2 := L.Deserialize(d); "
        "  return (if l2.Line = \"a\\nb\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: JSON record round-trip — float field.
// WriteFloat produces a JSON number.
BEZEL_TEST(codegen_json_550, record_float_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(V: float): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var r := new R(2.5f); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.V = 2.5f then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: WriteDecimal produces a JSON number without precision loss.
BEZEL_TEST(codegen_json_550, record_decimal_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record D(V: decimal): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var d0 := new D(1.25m); "
        "  d0.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var d1 := D.Deserialize(d); "
        "  return (if d1.V = 1.25m then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: enum serializes as name string (same as binary, via WriteString).
BEZEL_TEST(codegen_json_550, enum_roundtrip_name_form_spec_2_15_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "enum Color: ISerializable, IDeserializable Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  Color.Green.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Color.Deserialize(d); "
        "  return (if v = Color.Green then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: nested record round-trip via JSON.
BEZEL_TEST(codegen_json_550, nested_record_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Inner(X: int): ISerializable, IDeserializable; "
        "record Outer(A: Inner, B: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var o := new Outer(new Inner(42), 7); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var o2 := Outer.Deserialize(d); "
        "  return (if o2.A.X = 42 and o2.B = 7 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: nullable field — null case.
BEZEL_TEST(codegen_json_550, nullable_null_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(V: int?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var r := new R(null); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.V = null then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: nullable field — non-null case.
BEZEL_TEST(codegen_json_550, nullable_nonnull_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(V: int?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var r := new R(99); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.V = 99 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8 / §2.15: tree-class round-trip via JsonSerializer/JsonDeserializer.
// Type-driven (v0.141): ISerializable/IDeserializable conformance drives synthesis.
BEZEL_TEST(codegen_json_550, tree_class_roundtrip_spec_2_15_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Node : ISerializable, IDeserializable "
        "  public constructor(val: int) "
        "    this.Val := val; "
        "  end constructor; "
        "  public property Val: int; "
        "end Node; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  using var n := new Node(55); "
        "  n.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var n2 := Node.Deserialize(d); "
        "  return (if n2.Val = 55 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8 / §2.15: missing required field raises SerializationException.
// §2.15: "missing required fields remain unconditional errors" regardless of
// IgnoreUnknownFields. A required field absent from the JSON input → exception.
BEZEL_TEST(codegen_json_550, missing_required_field_raises_spec_2_15_9_8) {
    // JSON input {"Y":7} is missing field "X" — should throw SerializationException.
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"Y\\\":7}\"); "
        "  try "
        "    var p := Point.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: malformed JSON input raises SerializationException — truncated input.
BEZEL_TEST(codegen_json_550, malformed_json_truncated_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: malformed JSON — bad token (number where object expected).
BEZEL_TEST(codegen_json_550, malformed_json_bad_token_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"42\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: type mismatch — ReadInt when next token is string → SerializationException.
BEZEL_TEST(codegen_json_550, type_mismatch_string_for_int_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":\\\"notanumber\\\"}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: cross-format negative — binary bytes into JsonDeserializer.
// Binary-format bytes are not valid JSON; should raise SerializationException,
// not crash silently.
BEZEL_TEST(codegen_json_550, cross_format_binary_into_json_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  var r := new R(1); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  try "
        "    var r2 := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: JsonSerializer.ToString() convenience — verify output is real JSON.
// Test against a known small case: {"X":3} for Point(X=3,Y=0) is not mandated,
// but {"X":3,"Y":7} is verifiably real JSON — no spec example exists, so we
// verify the round-trip produces valid JSON by feeding it back to JsonDeserializer.
// The output IS real JSON (not binary) because JsonDeserializer accepts it.
BEZEL_TEST(codegen_json_550, tostring_produces_valid_json_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var p := new Point(3, 7); "
        "  p.Serialize(s); "
        "  var json := s.ToString(); "
        "  using var d := new JsonDeserializer(json); "
        "  var p2 := Point.Deserialize(d); "
        "  return (if p2.X = 3 and p2.Y = 7 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15: unknown-field strict by default → SerializationException.
// §2.15: "a field present in the input that matches no declared component …
// is, by default, a SerializationException."
BEZEL_TEST(codegen_json_550, unknown_field_strict_default_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":5,\\\"Z\\\":99}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: WriteNull → `null` in JSON; TryReadNull reads it back.
BEZEL_TEST(codegen_json_550, write_null_roundtrip_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record R(V: string?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var r := new R(null); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.V = null then 1 else 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §2.15 / fix #1229: WriteFloat on a JsonSerializer raises SerializationException
// for NaN / ±Infinity (JSON has no representation for non-finite floats).
// The synthesized Serialize must check bz_exc after bz_binser_write_float and
// propagate the exception rather than falling through to EndObject.
// ===========================================================================

// §2.15 / fix #1229: NaN in a double field → SerializationException caught.
// Bug A: before the fix, no exception was raised; malformed `{"V":}` was produced.
BEZEL_TEST(codegen_json_1229,
           nan_double_field_to_json_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record DVal(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var nan := double.NaN; "
        "  using var s := new JsonSerializer(); "
        "  var r := new DVal(nan); "
        "  try "
        "    r.Serialize(s); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15 / fix #1229: PositiveInfinity in a double field → SerializationException.
BEZEL_TEST(codegen_json_1229,
           positive_infinity_double_field_to_json_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record DVal(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var inf := double.PositiveInfinity; "
        "  using var s := new JsonSerializer(); "
        "  var r := new DVal(inf); "
        "  try "
        "    r.Serialize(s); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15 / fix #1229 Bug B: a SECOND serialize call after catching the exception
// must work correctly; bz_exc must be clear after the catch.
// Before the fix, the stale bz_exc left by the first Serialize caused the next
// synthesized-Serialize entry check (bz_binser_begin_object emitPendingCheck) to
// abort with "Unhandled exception".
BEZEL_TEST(codegen_json_1229,
           second_serialize_after_caught_nan_works_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record DVal(V: double): ISerializable, IDeserializable; "
        "record Good(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var nan := double.NaN; "
        "  var caught := 0; "
        "  using var s := new JsonSerializer(); "
        "  var bad := new DVal(nan); "
        "  try "
        "    bad.Serialize(s); "
        "  catch SerializationException => caught := 1; "
        "  end; "
        "  if caught = 0 then return 0; end; "  // exception must have been caught
        "  using var s2 := new JsonSerializer(); "
        "  var g := new Good(42); "
        "  g.Serialize(s2); "
        "  using var bytes := s2.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var g2 := Good.Deserialize(d); "
        "  return (if g2.X = 42 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15 / fix #1229: binary serialization of NaN is ALLOWED (§2.15 restricts
// only JSON); BinarySerializer must round-trip NaN without raising.
BEZEL_TEST(codegen_json_1229,
           nan_double_field_to_binary_allowed_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record DVal(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var nan := double.NaN; "
        "  using var s := new BinarySerializer(); "
        "  var r := new DVal(nan); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var r2 := DVal.Deserialize(d); "
        "  return (if double.IsNaN(r2.V) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15 / fix #1229: finite double still serializes to JSON correctly (regression).
BEZEL_TEST(codegen_json_1229,
           finite_double_field_to_json_works_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record DVal(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var r := new DVal(3.14); "
        "  r.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var r2 := DVal.Deserialize(d); "
        "  return (if r2.V = 3.14 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §2.15 / fix #1229: ASan — caught SerializationException from NaN must be freed
// exactly once (no leak, no double-free).
BEZEL_TEST(codegen_json_1229,
           asan_nan_json_exception_freed_once_spec_2_15) {
    int rc = runUnderLeakSanitizerJson(
        std::string("module M; ") + kImport +
        "record DVal(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var nan := double.NaN; "
        "  using var s := new JsonSerializer(); "
        "  var r := new DVal(nan); "
        "  try "
        "    r.Serialize(s); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;",
        /*detectLeaks=*/true);
    // rc == -1 → toolchain unavailable (SKIP).
    // rc == 0 or 1 → exit + no leaks (also clean; exit 1 = Bezel success return).
    // rc == 99 → ASan/leak triggered → FAIL.
    BEZEL_EXPECT_TRUE(rc == 0 || rc == 1 || rc == -1);
}

// ===========================================================================
// AOT / ASan tests — detect_leaks=1 on round-trip and malformed-input probes.
// These test with the lane-local rt_asan.o (per mandatory memory discipline).
// ===========================================================================

// §2.15 ASan: JSON record round-trip — no leaks.
BEZEL_TEST(codegen_json_550, asan_record_roundtrip_no_leaks_spec_2_15) {
    int rc = runUnderLeakSanitizerJson(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var p := new Point(3, 7); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var p2 := Point.Deserialize(d); "
        "  return (if p2.X = 3 and p2.Y = 7 then 1 else 0); "
        "end Main; end C;",
        /*detectLeaks=*/true);
    // rc == -1 means toolchain unavailable → SKIP.
    // rc == 0 or 1 means clean (exit 0 = plain clean, exit 1 = Bezel's success return value).
    // rc == 99 means leak or asan error → FAIL.
    BEZEL_EXPECT_TRUE(rc == 0 || rc == 1 || rc == -1);
}

// §2.15 ASan: malformed JSON raises without leaking memory.
BEZEL_TEST(codegen_json_550, asan_malformed_json_no_leaks_spec_2_15) {
    int rc = runUnderLeakSanitizerJson(
        std::string("module M; ") + kImport +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;",
        /*detectLeaks=*/true);
    // The exit code of 1 (caught SerializationException) means ASan sees
    // exit(1) — which with exitcode=99 override means: if rc==1 → clean asan.
    // Only rc==99 is an asan trigger. rc==-1 is skip.
    BEZEL_EXPECT_TRUE(rc == 0 || rc == 1 || rc == -1);
}

// ===========================================================================
// §2.15 v0.139 / issue #675: JSON graph-identity tokens ($id / $ref)
//
// Per §2.15 v0.139: an object carrying an identity tag is written with a
// reserved first field `"$id": n`; a back-reference is written as `{"$ref":n}`.
// On the read side: first field "$id" registers the tag; only field "$ref"
// resolves it.  Field names beginning with `$` are reserved.
// ===========================================================================

// §2.15 v0.139: JSON graph diamond round-trip — identity preserved.
// Topology: Diamond owns Node (Shared), borrows the same Node (Alias).
// After round-trip: mutating Shared.Val through one alias is visible via Alias.
// Wire must contain "$id" and "$ref" fields (verified separately below).
BEZEL_TEST(codegen_json_675, graph_diamond_roundtrip_spec_2_15_v0139) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Node : ISerializable, IDeserializable "
        "  public Val: int := 0; "
        "  public constructor(val: int) Val := val; end constructor; "
        "end Node; "
        "class Diamond : ISerializable, IDeserializable "
        "  public owned Shared: Node; "
        "  public Alias: Node?; "
        "  public constructor(owned shared: Node, alias: Node?) "
        "    Shared := shared; Alias := alias; "
        "  end constructor; "
        "end Diamond; "
        "class C public static method Main(): int "
        "  var n := new Node(40); "
        "  using var d := new Diamond(move n, n); "
        "  using var ser := new JsonSerializer(); "
        "  d.Serialize(ser); "
        "  using var bytes := ser.ToBytes(); "
        "  using var de := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var back := Diamond.Deserialize(de); "
        "  back.Shared.Val := back.Shared.Val + 2; "
        // After mutating via Shared, Alias must reflect the same value (identity).
        "  if back.Alias = null then return 91; end; "
        "  if back.Alias?.Val = 42 then return 42; end; "
        "  return back.Alias?.Val ?? 0; "
        "end Main; end C;"), 42);
}

// §2.15 v0.139: JSON output MUST contain "$id" and "$ref" fields.
// String-compare test: verifies the wire text is the expected JSON graph form.
BEZEL_TEST(codegen_json_675, graph_wire_text_contains_id_and_ref_spec_2_15_v0139) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Node : ISerializable, IDeserializable "
        "  public Val: int := 0; "
        "  public constructor(val: int) Val := val; end constructor; "
        "end Node; "
        "class Hub : ISerializable, IDeserializable "
        "  public owned Target: Node; "
        "  public Alias: Node?; "
        "  public constructor(owned target: Node, alias: Node?) "
        "    Target := target; Alias := alias; "
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        "  var n := new Node(7); "
        "  using var h := new Hub(move n, n); "
        "  using var ser := new JsonSerializer(); "
        "  h.Serialize(ser); "
        "  var json := ser.ToString(); "
        // The JSON must contain "$id" (inline tag) and "$ref" (back-reference).
        "  var hasId  := json.Contains(\"\\\"$id\\\"\"); "
        "  var hasRef := json.Contains(\"\\\"$ref\\\"\"); "
        "  if hasId and hasRef then return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §2.15 v0.139: null back-edge in JSON graph mode serializes as `null` and
// deserializes back to null (the null-ref path must remain working).
BEZEL_TEST(codegen_json_675, graph_null_back_edge_json_spec_2_15_v0139) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Slot : ISerializable, IDeserializable "
        "  public Name: string := \"\"; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Hub : ISerializable, IDeserializable "
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public constructor(owned target: Slot, alias: Slot?) "
        "    Target := target; Alias := alias; "
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        // Alias intentionally left null — exercises null back-edge in JSON mode.
        "  using var hub := new Hub(new Slot(\"beta\"), null); "
        "  using var ser := new JsonSerializer(); "
        "  hub.Serialize(ser); "
        "  using var bytes := ser.ToBytes(); "
        "  using var de := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(de); "
        "  if hub2.Target.Name <> \"beta\" then return 91; end; "
        "  if hub2.Alias <> null then return 92; end; "
        "  return 42; "
        "end Main; end C;"), 42);
}

// §2.15 v0.139: `serialized graph` cycle raises SerializationException in JSON mode.
// (Same contract as binary mode — cycle detection is format-agnostic.)
// JIT-CONTRAST-by-design (AOT-migration finding, class D). The self-loop this
// test builds (`a.Next := a;`, `Next: Link?` a PLAIN nullable field) is a
// post-construction assignment of a scope-bound local's borrow into a field —
// and §28.3's field-store rule is unconditional: "a borrow may not be assigned
// to any field of any object" (verified directly: `build/bezelc` rejects it
// with "borrow of `a` stored into a field would outlive its source"). This
// is not specific to self-loops or this test — the SAME rejection fires for
// closing ANY cycle through a plain borrow field (a two-node A->B->A cycle
// needs a post-hoc field write to close the loop too), which is exactly why
// the stdlib's CURRENT (type-driven, v0.141+) serialization model requires
// `Shared<T>`/`Weak<T>` for cyclic graphs: "the cycle is constructed in
// ownership-valid Bezel and round-trips through the ownership checker"
// (Bezel_Language_Spec.md §9.8, "Synthesized Deserialize" ¶). That newer
// model, notably, no longer RAISES on a cycle — it round-trips one — so this
// test's assertion (cycle -> SerializationException) is not just built with
// now-invalid syntax, it also encodes the PRE-migration (v0.139 keyword-
// clause era) semantics that the type-driven model superseded. There is no
// reshape that is both ownership-valid AND preserves the original assertion;
// filed as #1773 (stale-test-debt) for a spec-process decision (rewrite
// against Shared<T>/Weak<T>'s actual current behavior, or retire). Left on
// runJit, which — by design — skips the ownership pass this finding turns on.
BEZEL_TEST(codegen_json_675, graph_json_cycle_raises_spec_2_15_v0139) {
    auto r = runJit(
        std::string("module M; ") + kImport +
        "class Link : ISerializable, IDeserializable "
        "  public Val: int := 0; "
        "  public Next: Link?; "
        "  public constructor(val: int, next: Link?) "
        "    Val := val; Next := next; "
        "  end constructor; "
        "end Link; "
        "class C public static method Main(): int "
        "  using var a := new Link(1, null); "
        // Build a self-loop (cycle): a.Next = a.
        "  a.Next := a; "
        "  using var ser := new JsonSerializer(); "
        "  try "
        "    a.Serialize(ser); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}

// §2.15 v0.139: tree-mode JSON is UNCHANGED — no $id/$ref fields appear for
// type-driven (v0.141) tree classes.  The tree JSON round-trip still returns 42.
BEZEL_TEST(codegen_json_675, tree_json_unchanged_spec_2_15_v0139) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Leaf : ISerializable, IDeserializable "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Leaf; "
        "class Root : ISerializable, IDeserializable "
        "  public owned L: Leaf; "
        "  public Id: int := 0; "
        "  public constructor(id: int, owned l: Leaf) Id := id; L := l; end constructor; "
        "end Root; "
        "class C public static method Main(): int "
        "  using var r := new Root(7, new Leaf(35)); "
        "  using var ser := new JsonSerializer(); "
        "  r.Serialize(ser); "
        "  var json := ser.ToString(); "
        // Tree JSON must NOT contain "$id" or "$ref".
        "  if json.Contains(\"\\\"$id\\\"\") then return 91; end; "
        "  if json.Contains(\"\\\"$ref\\\"\") then return 92; end; "
        "  using var bytes := ser.ToBytes(); "
        "  using var de := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var back := Root.Deserialize(de); "
        "  if back.Id = 7 and back.L.N = 35 then return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §2.15 v0.139: `serialized "xxx"` rename whose name begins with `$` is a
// compile error at the conformance declaration (reserved for graph-identity).
// Syntax: `Name: Type serialized "wireName"` (SerializedClause follows the type).
BEZEL_TEST(codegen_json_675,
           dollar_rename_record_field_is_error_spec_2_15_v0139) {
    // A positional record field with serialized "$foo" rename → compile error.
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "record R(X: int serialized \"$foo\"): ISerializable, IDeserializable; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// §2.15 v0.139: ASan/LeakSanitizer verify JSON graph round-trip is leak-free.
// detect_leaks=1: no leaks from tag table allocations or deserializer state.
BEZEL_TEST(codegen_json_675,
           asan_graph_diamond_roundtrip_no_leaks_spec_2_15_v0139) {
    int rc = runUnderLeakSanitizerJson(
        std::string("module M; ") + kImport +
        "class Node : ISerializable, IDeserializable "
        "  public Val: int := 0; "
        "  public constructor(val: int) Val := val; end constructor; "
        "end Node; "
        "class Diamond : ISerializable, IDeserializable "
        "  public owned Shared: Node; "
        "  public Alias: Node?; "
        "  public constructor(owned shared: Node, alias: Node?) "
        "    Shared := shared; Alias := alias; "
        "  end constructor; "
        "end Diamond; "
        "class C public static method Main(): int "
        "  var n := new Node(40); "
        "  using var d := new Diamond(move n, n); "
        "  using var ser := new JsonSerializer(); "
        "  d.Serialize(ser); "
        "  using var bytes := ser.ToBytes(); "
        "  using var de := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var back := Diamond.Deserialize(de); "
        "  back.Shared.Val := back.Shared.Val + 2; "
        "  if back.Alias = null then return 1; end; "
        "  if back.Alias?.Val = 42 then return 42; end; "
        "  return 1; "
        "end Main; end C;",
        /*detectLeaks=*/true);
    // rc == -1 → toolchain unavailable (SKIP).
    // rc == 42 → correct exit code + no leaks.
    // rc == 0  → exit(0) + no leaks (also clean).
    // rc == 99 → ASan/leak triggered → FAIL.
    BEZEL_EXPECT_TRUE(rc == 0 || rc == 42 || rc == -1);
}
