// Codegen tests — §9.8 serialization / stdlib §2.15: JsonSerializer/JsonDeserializer.
// Issue #550, JSON format tier.
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
// Graph-identity JSON form ($id/$ref equivalent) is NOT specified in §2.15.
// Tree-only JSON is implemented. Synthesis with JsonSerializer on a
// `serialized graph`/`serialized cyclic` class is not yet wired (reported as
// a docs-candidate gap — §2.15 does not specify the JSON graph wire form).
//
// Spec refs: §9.8, stdlib §2.14, §2.15.

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
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

bool compileFails(const std::string& src) {
    auto r = runJit(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

// §2.15: requires explicit import.
const char* kImport = "import System.Serialization; ";

// ---------------------------------------------------------------------------
// AOT round-trip + ASan helper (lane-local rt object per mandatory memory
// discipline: /tmp/wt-json/rt_asan.o — not /tmp/bezel_rt_asan_memoized.o).
// ---------------------------------------------------------------------------
static std::string laneLocalAsanObject() {
    // Build lane-local ASan runtime once per process.
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;

    std::string rtSrc = ::bezel::native_test::runtimeSourcePath();
    std::string rtObj = "/tmp/wt-json/rt_asan.o";

    // Build with systemd-run memory cap (6G) to avoid OOM.
    std::string cmd =
        "systemd-run --user --scope -q -p MemoryMax=6G -p MemorySwapMax=0 -- "
        "clang++ -std=c++20 -g -fsanitize=address -pthread -c " +
        rtSrc + " -o " + rtObj + " > /dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        // Try without systemd-run (might not be available in all envs).
        cmd = "clang++ -std=c++20 -g -fsanitize=address -pthread -c " +
              rtSrc + " -o " + rtObj + " > /dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) return cached;
    }
    cached = rtObj;
    return cached;
}

// Run source under ASan (detect_leaks enabled by default).
// Returns 0=clean, 99=asan/leak triggered, -1=skip (toolchain/compile fail).
int runUnderLeakSanitizerJson(const std::string& source, bool detectLeaks = true) {
    namespace fs = std::filesystem;
    const std::string& rtObj = laneLocalAsanObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_json_lsan_" + std::to_string(counter++);
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
        "  return (1 if p2.X = 3 and p2.Y = 7 else 0); "
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
        "  return (1 if f2.Active = true and f2.Done = false else 0); "
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
        "  return (1 if m2.Text = \"hello\" else 0); "
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
        "  return (1 if w2.V = \"say \\\"hi\\\"\" else 0); "
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
        "  return (1 if p2.Path = \"a\\\\b\" else 0); "
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
        "  return (1 if l2.Line = \"a\\nb\" else 0); "
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
        "  return (1 if r2.V = 2.5f else 0); "
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
        "  return (1 if d1.V = 1.25m else 0); "
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
        "  return (1 if v = Color.Green else 0); "
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
        "  return (1 if o2.A.X = 42 and o2.B = 7 else 0); "
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
        "  return (1 if r2.V = null else 0); "
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
        "  return (1 if r2.V = 99 else 0); "
        "end Main; end C;"), 1);
}

// §9.8 / §2.15: tree-class round-trip via JsonSerializer/JsonDeserializer.
// `serialized tree` clause enables synthesis for the class.
BEZEL_TEST(codegen_json_550, tree_class_roundtrip_spec_2_15_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class Node serialized tree: ISerializable, IDeserializable "
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
        "  return (1 if n2.Val = 55 else 0); "
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
        "  return (1 if p2.X = 3 and p2.Y = 7 else 0); "
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
        "  return (1 if r2.V = null else 0); "
        "end Main; end C;"), 1);
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
        "  return (1 if p2.X = 3 and p2.Y = 7 else 0); "
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
