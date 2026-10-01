// Spec-derived codegen tests — fix #668 and fix #669:
//
//   #668: BinaryDeserializer silently defaults missing required fields (§2.15/§9.8).
//         A component that is neither `serialized skip` nor `serialized optional`
//         MUST be present in the wire data; absence is a structural error and must
//         raise SerializationException unconditionally (stdlib §2.15: "missing
//         required fields remain unconditional errors"; spec §9.8: "a missing
//         required component … raises SerializationException unconditionally").
//
//   #669: AOT-only SIGSEGV deserializing a NULL nullable-class field (§9.8/§28).
//         The synthesized Deserialize for a class whose field is an owned Child?
//         crashed in native (AOT) mode when the null marker was on the wire.
//         Root cause: the inner emitClassDeserialize's namePtrSlot alloca was
//         null-initialized in the live path only; on the null path the alloca
//         contained stack garbage which was later released via the disposeStack_
//         stringRelease mechanism, causing SIGSEGV in bz_string_release.
//
// Spec refs: §9.8 (v0.135+), stdlib §2.15.
// JIT harness returns Main's int exit code. AOT probes use compileAndRun668.

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
#include "support/unique_stem.hpp"

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

// Compile `source` to a temp native executable and run it;
// returns the process exit code, or -1 if compilation/link/run failed.
// per §33.3: Main's return value is the process exit code.
int compileAndRun668(const std::string& source) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_668_aot");
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

// Compile to native object, link with ASan runtime, run with detect_leaks.
// Returns 0 (clean), 1 (sanitizer triggered), -1 (toolchain unavailable/skip).
int runUnderLeakSanitizer668(const std::string& source, bool detectLeaks = true) {
    namespace fs = std::filesystem;
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_668_lsan");
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
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                       // clean
}

const char* kImport = "import System.Serialization; ";

} // namespace

// ===========================================================================
// #668 — missing required field must raise SerializationException (§9.8/§2.15)
//
// Stdlib §2.15: "missing required fields remain unconditional errors".
// Spec §9.8: "A missing required component … raises SerializationException
// unconditionally — these are structural faults in the input regardless
// of policy."
//
// Test: serialize a TwoField class (A:int, B:int); deserialize as ThreeField
// (A:int, B:int, C:int). C is absent from the wire → SerializationException.
// ===========================================================================

// §9.8 / §2.15 / #668: missing required field (class) must raise
// SerializationException, not silently default to 0.
BEZEL_TEST(codegen_serialization_668,
           missing_required_field_class_raises_spec_9_8_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "class TwoField : ISerializable, IDeserializable "
        "  public A: int := 0; "
        "  public B: int := 0; "
        "  public constructor(a: int, b: int) A := a; B := b; end constructor; "
        "end TwoField; "
        "class ThreeField : ISerializable, IDeserializable "
        "  public A: int := 0; "
        "  public B: int := 0; "
        "  public C: int := 0; "
        "  public constructor(a: int, b: int, c: int) A := a; B := b; C := c; "
        "end constructor; "
        "end ThreeField; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new TwoField(1, 2); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var caught := 0; "
        "    try "
        "      using var bad := ThreeField.Deserialize(de); "
        "      caught := 0; "
        "    catch SerializationException => caught := 1; "
        "    end; "
        "    return (if caught = 1 then 42 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// §9.8 / §2.15 / #668: same as above but for records (positional fields).
// Records synthesize Deserialize via emitRecordDeserialize which also needs
// the missing-field check.
BEZEL_TEST(codegen_serialization_668,
           missing_required_field_record_raises_spec_9_8_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record TwoFR(A: int, B: int): ISerializable, IDeserializable; "
        "record ThreeFR(A: int, B: int, C: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        "    var src := new TwoFR(10, 20); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var caught := 0; "
        "    try "
        "      var bad := ThreeFR.Deserialize(de); "
        "    catch SerializationException => caught := 1; "
        "    end; "
        "    return (if caught = 1 then 42 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// §9.8 / §2.15 / #668: `serialized optional` field is EXEMPT — missing optional
// field must silently default (not raise).
// §9.8: "a missing field yields default(T) instead of raising SerializationException"
BEZEL_TEST(codegen_serialization_668,
           missing_optional_field_defaults_not_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "class Base : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Base; "
        // Extended has an extra optional nullable string property (uses property keyword).
        "class Extended : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public property Note: string? serialized optional; "
        "  public constructor(v: int, note: string?) V := v; Note := note; end constructor; "
        "end Extended; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new Base(99); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        // Deserialize as Extended — V is present, Note is absent → must default to null
        "    var caught := 0; "
        "    using var ext := Extended.Deserialize(de); "
        "    if ext.V <> 99 then return 91; end; "
        "    return (if ext.Note = null then 42 else 92); "
        "  end Main; "
        "end Program;"), 42);
}

// §9.8 / §2.15 / #668: a nullable T? field WITHOUT `serialized optional` is
// REQUIRED — its absence is a SerializationException (null on wire is distinct
// from absent on wire; a null T? writes a null marker, which is present).
// §9.8: "serialized optional is the per-field missing-field tolerant-mode opt-in;
// a nullable T? without serialized optional is still required."
// (implied by: "missing required component … raises SerializationException
// unconditionally" — nullability does not confer optionality).
BEZEL_TEST(codegen_serialization_668,
           missing_nullable_required_field_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "class NoTag : ISerializable, IDeserializable "
        "  public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; "
        "end NoTag; "
        // HasTag has an extra required nullable property.
        "class HasTag : ISerializable, IDeserializable "
        "  public V: int := 0; "
        "  public Tag: string? := null; "
        "  public constructor(v: int, tag: string?) V := v; Tag := tag; end constructor; "
        "end HasTag; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new NoTag(5); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var caught := 0; "
        "    try "
        "      using var bad := HasTag.Deserialize(de); "
        "      caught := 0; "
        "    catch SerializationException => caught := 1; "
        "    end; "
        "    return (if caught = 1 then 42 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// ===========================================================================
// #669 — AOT null nullable-class field SIGSEGV (§9.8/§28)
//
// Root cause: emitClassDeserialize's inner namePtrSlot (for the live path
// of a nullable class field) was null-initialized in liveBB only; on the
// null path the alloca held uninitialized stack bytes → SIGSEGV in
// bz_string_release when the using-var scope exited.
//
// Fix: initialize namePtrSlot in the function entry block (not the live-path
// block) so it is always null even when the null path is taken at runtime.
// ===========================================================================

// §9.8 / #669: AOT native round-trip — serialize Parent(tag=5, null) and
// deserialize; null nullable-class field must survive AOT without SIGSEGV.
// Exact reproduction of r44e_serial_nullable_exact_test_code.bzl.
BEZEL_TEST(codegen_serialization_669,
           aot_null_nullable_class_field_no_sigsegv_spec_9_8) {
    const std::string src =
        std::string("module M; ") + kImport +
        "class Child : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Child; "
        "class Parent : ISerializable, IDeserializable "
        "  public property Tag: int; "
        "  public owned OptChild: Child?; "
        "  public constructor(tag: int, owned optChild: Child?) "
        "    Tag := tag; OptChild := optChild; "
        "  end constructor; "
        "end Parent; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Parent(5, null); "
        "    using var s := new BinarySerializer(); "
        "    p.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    using var p2 := Parent.Deserialize(d); "
        "    if p2.Tag <> 5 then return 91; end; "
        "    return (if p2.OptChild = null then 42 else 92); "
        "  end Main; "
        "end C;";
    // AOT run: must return 42 without SIGSEGV (rc=139).
    BEZEL_EXPECT_EQ(compileAndRun668(src), 42);
}

// §9.8 / #669: AOT native round-trip — non-null nullable-class field must
// also work (regression guard to confirm the live path still deserializes).
BEZEL_TEST(codegen_serialization_669,
           aot_nonnull_nullable_class_field_roundtrip_spec_9_8) {
    const std::string src =
        std::string("module M; ") + kImport +
        "class Child : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Child; "
        "class Parent : ISerializable, IDeserializable "
        "  public property Tag: int; "
        "  public owned OptChild: Child?; "
        "  public constructor(tag: int, owned optChild: Child?) "
        "    Tag := tag; OptChild := optChild; "
        "  end constructor; "
        "end Parent; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Parent(7, new Child(99)); "
        "    using var s := new BinarySerializer(); "
        "    p.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    using var p2 := Parent.Deserialize(d); "
        "    if p2.Tag <> 7 then return 91; end; "
        "    var oc := p2.OptChild; "
        "    if oc = null then return 92; end; "
        "    return (if oc.V = 99 then 42 else 93); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(compileAndRun668(src), 42);
}

// §9.8 / #669: ASan verify — null nullable-class path leaves no leaks or
// double-frees.  Requires clang++ in PATH; skipped silently if unavailable.
BEZEL_TEST(codegen_serialization_669,
           asan_null_nullable_class_field_no_leak_spec_9_8) {
    const std::string src =
        std::string("module M; ") + kImport +
        "class Child : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Child; "
        "class Parent : ISerializable, IDeserializable "
        "  public property Tag: int; "
        "  public owned OptChild: Child?; "
        "  public constructor(tag: int, owned optChild: Child?) "
        "    Tag := tag; OptChild := optChild; "
        "  end constructor; "
        "end Parent; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Parent(5, null); "
        "    using var s := new BinarySerializer(); "
        "    p.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    using var p2 := Parent.Deserialize(d); "
        "    if p2.Tag <> 5 then return 91; end; "
        "    return (if p2.OptChild = null then 42 else 92); "
        "  end Main; "
        "end C;";
    int asan = runUnderLeakSanitizer668(src, /*detectLeaks=*/true);
    if (asan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(asan, 0);
}

// §9.8 / #669: ASan verify — truncated mid-object probe (Child deserialized
// when null-path interrupted by truncated data) leaves no leaks.
// This exercises the exception path in the inner emitClassDeserialize.
BEZEL_TEST(codegen_serialization_669,
           asan_truncated_nullable_class_no_leak_spec_9_8) {
    // Serialize Parent(7, new Child(99)); truncate buffer in half; deserialize.
    // Should raise SerializationException (caught → 42), no leak.
    const std::string src =
        std::string("module M; ") + kImport +
        "class Child : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Child; "
        "class Parent : ISerializable, IDeserializable "
        "  public property Tag: int; "
        "  public owned OptChild: Child?; "
        "  public constructor(tag: int, owned optChild: Child?) "
        "    Tag := tag; OptChild := optChild; "
        "  end constructor; "
        "end Parent; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Parent(7, new Child(99)); "
        "    using var s := new BinarySerializer(); "
        "    p.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    var halfLen: int := bytes.Length / 2; "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, halfLen)); "
        "    var caught := false; "
        "    try "
        "      using var p2 := Parent.Deserialize(d); "
        "      return 0; "
        "    catch SerializationException => caught := true; "
        "    end; "
        "    return (if caught then 42 else 1); "
        "  end Main; "
        "end C;";
    // JIT: exception is caught → 42.
    BEZEL_EXPECT_EQ(run(src), 42);
    // ASan: no leak/UAF/double-free.
    int asan = runUnderLeakSanitizer668(src, /*detectLeaks=*/true);
    if (asan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(asan, 0);
}
