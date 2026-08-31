// Spec-derived codegen tests — fix #1397 and fix #1390 (§2.14 / §2.15 / §9.8).
//
//   #1397: the concrete BinarySerializer / JsonSerializer write methods and the
//          concrete BinaryDeserializer / JsonDeserializer read methods were not
//          wired from codegen to the bz_binser_* / bz_binder_* runtime.  A direct
//          `ser.WriteInt(x)` on a `BinarySerializer` (as opposed to via an
//          `ISerializer` receiver) was silently dropped, so `ToBytes()` returned
//          an empty buffer (Length 0); `JsonDeserializer("42").ReadInt()` read a
//          0 sentinel.  Root cause: tryLowerSerializationCall's concrete-type
//          blocks handled only ToBytes/ToString/Dispose and returned nullptr for
//          every other method (isSerializationCall still flagged the call, so the
//          write/read was consumed as a no-op).  The fix routes the concrete
//          receivers through the same bz_binser_*/bz_binder_* lowering the
//          interface receivers already used, and sema types the concrete-class
//          read methods (ReadInt→long, ReadString→string, …).
//
//   #1390: an `s: ISerializer` parameter (and a `BinarySerializer` local / field /
//          return) lowered to i32 in lowerTypeRef — the serializer/deserializer
//          builtin type names were absent from the heap-pointer whitelist, so they
//          hit the i32 value-kind fallback.  The opaque serializer pointer was
//          ABI-coerced to its low 32 bits crossing the hand-written `Serialize`
//          call boundary and zero-extended back inside the callee, giving a
//          0x00000000_xxxxxxxx receiver.  At low heap addresses the truncation is
//          lossless (the regular JIT suite passes); under ASan's high address
//          space the high 32 bits are lost, so bz_binser_begin_object dereferenced
//          a wild pointer and SEGV'd.  The AOT path caught it even without ASan:
//          the ptr→i32 mismatch fails LLVM verification, so emitObjectFile failed.
//          The fix lowers ISerializer/IDeserializer/ISerializable/IDeserializable
//          and the four concrete serializer/deserializer classes to ptr.
//
// The #1390 regression pin is prefix-free and needs no ASan: the hand-written
// `Serialize(s: ISerializer)` program compiles to a NATIVE object (which runs the
// LLVM verifier) and runs, returning 42.  Pre-fix the AOT compile failed the
// verifier (ptr passed to an i32 parameter), so compileAndRun returned -1.
//
// All APIs verified against Bezel_Standard_Library.md (ISerializer/IDeserializer
// §2.14; BinarySerializer/BinaryDeserializer/JsonSerializer/JsonDeserializer
// §2.15).  BinaryDeserializer takes a ReadOnlyArraySpan<byte> (§1358), so the
// round-trips feed it `buf.AsReadOnlySpan()`.
//
// Spec refs: §2.14, §2.15 (v0.165), §9.8.  Issues #1397, #1390.

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

const char* kImport = "import System.Serialization; ";

// JIT: return Main's exit code, or -999 if it did not run.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// AOT: compile `source` to a native executable and run it; returns the process
// exit code, or -1 if compile/link/run failed.  per §33.3: Main's return value
// is the process exit code.  This path runs the LLVM verifier during
// emitObjectFile, so a ptr/i32 signature mismatch (the #1390 defect) fails here.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_1390_aot_" + std::to_string(counter++);
    std::string src  = (dir / (stem + ".bzl")).string();
    std::string exe  = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

// Compile to a native object, link with the ASan runtime, run under ASan.
// Returns 0 (clean), 1 (sanitizer triggered), -1 (toolchain unavailable → skip).
// ASan places allocations high in the address space, so this is the direct
// reproduction of the #1390 crash condition without needing a long test prefix.
int runUnderAsan1390(const std::string& source) {
    namespace fs = std::filesystem;
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1390_asan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    // detect_leaks=0: the program intentionally exercises the serializer only.
    // exitcode=99 lets us distinguish a sanitizer abort from Main's own code.
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=0:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                       // clean
}

// ── #1397: BinarySerializer write → ToBytes → BinaryDeserializer read ─────────

// Repro A (verbatim from #1397): WriteInt then ToBytes must yield a non-empty
// buffer — a `long` is 8 bytes on the compact binary wire (§2.15).  Was 0.
BEZEL_TEST(serialization_roundtrip_1397, binser_writeint_tobytes_length) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var ser := new BinarySerializer(); "
        "  ser.WriteInt(1234); "
        "  using var buf := ser.ToBytes(); "
        "  return buf.Length; "
        "end Main; end C;"), 8);
}

// int round-trip: WriteInt(42) → ToBytes → BinaryDeserializer.ReadInt() = 42.
BEZEL_TEST(serialization_roundtrip_1397, binary_int_roundtrip) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var ser := new BinarySerializer(); "
        "  ser.WriteInt(42); "
        "  using var buf := ser.ToBytes(); "
        "  using var d := new BinaryDeserializer(buf.AsReadOnlySpan()); "
        "  return int(d.ReadInt()); "
        "end Main; end C;"), 42);
}

// long round-trip: a value > 32 bits must survive the 8-byte binary encoding.
BEZEL_TEST(serialization_roundtrip_1397, binary_long_roundtrip) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  var big: long := 4294967296; "  // 2^32 — loses value if truncated to int
        "  using var ser := new BinarySerializer(); "
        "  ser.WriteInt(big); "
        "  using var buf := ser.ToBytes(); "
        "  using var d := new BinaryDeserializer(buf.AsReadOnlySpan()); "
        "  var ok: bool := d.ReadInt() = big; "
        "  return (if ok then 55 else 11); "
        "end Main; end C;"), 55);
}

// uint round-trip: WriteUInt(200) → ReadUInt() = 200.
BEZEL_TEST(serialization_roundtrip_1397, binary_uint_roundtrip) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var ser := new BinarySerializer(); "
        "  ser.WriteUInt(200u); "
        "  using var buf := ser.ToBytes(); "
        "  using var d := new BinaryDeserializer(buf.AsReadOnlySpan()); "
        "  return int(d.ReadUInt()); "
        "end Main; end C;"), 200);
}

// bool round-trip: WriteBool(true) → ReadBool() = true.
BEZEL_TEST(serialization_roundtrip_1397, binary_bool_roundtrip) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var ser := new BinarySerializer(); "
        "  ser.WriteBool(true); "
        "  using var buf := ser.ToBytes(); "
        "  using var d := new BinaryDeserializer(buf.AsReadOnlySpan()); "
        "  return (if d.ReadBool() then 7 else 9); "
        "end Main; end C;"), 7);
}

// string round-trip: WriteString → ReadString reads the same text back.
BEZEL_TEST(serialization_roundtrip_1397, binary_string_roundtrip) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var ser := new BinarySerializer(); "
        "  ser.WriteString(\"hello\"); "
        "  using var buf := ser.ToBytes(); "
        "  using var d := new BinaryDeserializer(buf.AsReadOnlySpan()); "
        "  var got := d.ReadString(); "
        "  return (if got = \"hello\" then 77 else 13); "
        "end Main; end C;"), 77);
}

// two values in sequence come back in order (offset advances correctly).
BEZEL_TEST(serialization_roundtrip_1397, binary_two_ints_in_order) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var ser := new BinarySerializer(); "
        "  ser.WriteInt(10); ser.WriteInt(32); "
        "  using var buf := ser.ToBytes(); "
        "  using var d := new BinaryDeserializer(buf.AsReadOnlySpan()); "
        "  var a := int(d.ReadInt()); "
        "  var b := int(d.ReadInt()); "
        "  return a + b; "
        "end Main; end C;"), 42);
}

// ── #1397: JSON serializer / deserializer ────────────────────────────────────

// Repro B (verbatim from #1397): JsonDeserializer("42").ReadInt() = 42.  Was 0.
BEZEL_TEST(serialization_roundtrip_1397, json_deserializer_readint_from_string) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"42\"); "
        "  return int(d.ReadInt()); "
        "end Main; end C;"), 42);
}

// JsonSerializer.WriteInt → ToString → JsonDeserializer round-trip.
BEZEL_TEST(serialization_roundtrip_1397, json_int_roundtrip_via_tostring) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "class C public static method Main(): int "
        "  using var ser := new JsonSerializer(); "
        "  ser.WriteInt(99); "
        "  var text := ser.ToString(); "
        "  using var d := new JsonDeserializer(text); "
        "  return int(d.ReadInt()); "
        "end Main; end C;"), 99);
}

// ── #1390: hand-written Serialize(s: ISerializer) — the ptr-not-i32 pin ───────

// The exact shape from tree_class_hand_written_serialize_suppresses_synthesis:
// a hand-written `Serialize(s: ISerializer)` calling `s.BeginObject(1)` etc.
// AOT compile (runs the verifier) + run must return 42.  Pre-fix the `s`
// parameter lowered to i32, so the call passed a ptr to an i32 parameter and
// the object failed LLVM verification → compileAndRun returned -1.
const char* kHandWrittenSerialize =
    "module M; import System.Serialization; "
    "class Box : ISerializable "
    "  public property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
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
    "  return (if bytes.Count > 0 then 42 else 91); "
    "end Main; end C;";

BEZEL_TEST(serialization_iserializer_ptr_1390, hand_written_serialize_aot_no_truncation) {
    BEZEL_EXPECT_EQ(compileAndRun(kHandWrittenSerialize), 42);
}

// The direct crash condition: same program under ASan (high-address allocations)
// must run clean (exit 0), not SEGV on the truncated receiver.  Skips cleanly
// (-1) when the ASan toolchain object is unavailable.
BEZEL_TEST(serialization_iserializer_ptr_1390, hand_written_serialize_asan_clean) {
    int r = runUnderAsan1390(kHandWrittenSerialize);
    if (r == -1) return;  // toolchain unavailable → skip
    BEZEL_EXPECT_EQ(r, 0);
}

// The JIT form also returns 42 (belt-and-suspenders; passes at low addresses even
// pre-fix, but guards against a future codegen regression that would also break
// the low-address path).
BEZEL_TEST(serialization_iserializer_ptr_1390, hand_written_serialize_jit) {
    BEZEL_EXPECT_EQ(run(kHandWrittenSerialize), 42);
}

}  // namespace
