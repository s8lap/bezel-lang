// Codegen + runtime — CString.ToBytes() round-trip (issue #1000, stdlib §10.1,
// v0.148). §10.1 declares `public method ToBytes(): owned Array<byte>` — it MUST
// return a FRESH, OWNED Array<byte> copy of the CString's bytes (count = the byte
// length EXCLUDING the null terminator, the same value §10.1 `Length` reports;
// elem_size 1). The result is INDEPENDENT of the CString's own buffer.
//
// Before the fix, `s.ToBytes()` was lowered to nothing (the call vanished): the
// inferred local `back` bound to a garbage/null value, `back.Count` read garbage,
// and — where the returned array's buffer aliased the CString — disposing BOTH
// the CString (free(char*)) and the Array<byte> (bz_array_free → free(data)) at
// scope end released the same allocation twice (free(): invalid pointer →
// SIGABRT). The data-copy half of #1000 (a collection literal in a byte-array FFI
// factory argument) was fixed separately by #999; THIS file covers the ToBytes
// round-trip half.
//
// The fix types `s.ToBytes()` as a concrete owned Array<byte> (sema) and lowers
// it to a new runtime helper bz_cstring_to_bytes that builds a fresh bz_array_t
// with a fresh malloc'd buffer holding a COPY of the strlen bytes. No aliasing →
// each owner (the CString and the Array<byte>) frees exactly once (§28.3).
//
// These tests verify (1) round-trip value correctness via BOTH factory paths
// (CString.FromBytes(b).ToBytes() and CString.From("hi").ToBytes()): Count and
// individual bytes; (2) ASan dispose-exactly-once across the round-trip,
// back-outliving-the-CString (returned from a method), and two ToBytes() on the
// same CString (two independent arrays); (3) CString.Length / From / FromBytes /
// disposal remain LSan-clean.

#include <cstdio>
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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1000() {
    return wtScratchDir("wt-1000");
}

// Compile + run natively; return exit code or -1 on failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir1000();
    std::string stem = unique_stem("bezel_1000");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
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

// Compile + link against the ASan runtime. Returns false on failure/unavailable.
bool lowerAndLink(const std::string& source, std::string& obj,
                  std::string& exe) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return false;
    auto dir = wtDir1000();
    std::string stem = unique_stem("bezel_1000_san");
    obj = (dir / (stem + ".o")).string();
    exe = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) { bezel::test::asanBuildFailure("parse"); return false; }
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) { bezel::test::asanBuildFailure("sema"); return false; }
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) { bezel::test::asanBuildFailure("emitModule"); return false; }
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, obj, err)) {
            bezel::test::asanBuildFailure("emitObjectFile");
            return false;
        }
    }
    std::string linkCmd = "clang++ -fsanitize=address -g " + obj + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(obj.c_str()); return false;
    }
    return true;
}

// LSan verification (detect_leaks=1). Returns 0 = clean, 1 = leak/corruption,
// -1 = skip (ASan runtime unavailable). Catches both a leak (the ToBytes buffer
// never freed) AND a double-free/UAF (the buffer aliased the CString).
int runUnderLeakSanitizer(const std::string& source) {
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return -1;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str()); std::remove(exe.c_str());
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Assert correct exit code AND LSan-clean (no leak, no UAF/double-free): the
// CString and the returned Array<byte> are EACH disposed exactly once.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// ---- Programs --------------------------------------------------------------

// stdlib §10.1: CString.FromBytes(b).ToBytes() round-trip — back.Count is 2.
const char* kFromBytesRoundTripCount =
    "module M; class C "
    "  public static method Main(): int "
    "    using var b: Array<byte> := [104, 105]; "
    "    using var s := CString.FromBytes(b); "
    "    using var back := s.ToBytes(); "
    "    return back.Count; "
    "  end Main; "
    "end C;";

// stdlib §10.1: the same round-trip, asserting the BYTE VALUES survive the copy
// (back[0] == 104, back[1] == 105). Returns 0 when every byte matches.
const char* kFromBytesRoundTripValues =
    "module M; class C "
    "  public static method Main(): int "
    "    using var b: Array<byte> := [104, 105]; "
    "    using var s := CString.FromBytes(b); "
    "    using var back := s.ToBytes(); "
    "    var bad: int := 0; "
    "    if back.Count <> 2 then bad := bad + 100; end; "
    "    if back[0] <> 104 then bad := bad + 10; end; "
    "    if back[1] <> 105 then bad := bad + 1; end; "
    "    return bad; "
    "  end Main; "
    "end C;";

// stdlib §10.1: CString.From("hi").ToBytes() — the From(string) factory path.
// "hi" is ASCII so its UTF-8 bytes are 104, 105. Count is 2.
const char* kFromStringRoundTripCount =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := CString.From(\"hi\"); "
    "    using var back := s.ToBytes(); "
    "    return back.Count; "
    "  end Main; "
    "end C;";

// stdlib §10.1: CString.From("hi").ToBytes() byte values — back[0]==104,
// back[1]==105. Returns 0 when every byte matches.
const char* kFromStringRoundTripValues =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := CString.From(\"hi\"); "
    "    using var back := s.ToBytes(); "
    "    var bad: int := 0; "
    "    if back.Count <> 2 then bad := bad + 100; end; "
    "    if back[0] <> 104 then bad := bad + 10; end; "
    "    if back[1] <> 105 then bad := bad + 1; end; "
    "    return bad; "
    "  end Main; "
    "end C;";

// ---- ASan dispose-once programs (looped to amplify any leak) ----------------

// stdlib §10.1 / §28.3: the FromBytes round-trip looped 1000× — the CString and
// the ToBytes Array<byte> are EACH disposed exactly once per iteration (no leak,
// no double-free of an aliased buffer). t accumulates 2 per iteration → 2000.
const char* kRoundTripLoopLeakFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do "
    "      using var b: Array<byte> := [104, 105]; "
    "      using var s := CString.FromBytes(b); "
    "      using var back := s.ToBytes(); "
    "      t := t + back.Count; i := i + 1; "
    "    end; "
    "    if t = 2000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// stdlib §10.1 / §28.3: the returned Array<byte> OUTLIVES the CString it came
// from — Make() builds a CString in an inner scope, returns ToBytes(), and the
// CString is disposed at Make()'s return while `back` (its independent copy)
// survives into Main. Looped to amplify: no UAF (back must own its buffer), no
// leak, no double-free. t accumulates 2 per iteration → 2000.
const char* kOutliveLoopLeakFree =
    "module M; class C "
    "  private static method Make(): owned Array<byte> "
    "    using var s := CString.From(\"hi\"); "
    "    return s.ToBytes(); "
    "  end Make; "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do "
    "      using var back := C.Make(); t := t + back.Count; i := i + 1; "
    "    end; "
    "    if t = 2000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// stdlib §10.1 / §28.3: TWO ToBytes() on the SAME CString produce TWO INDEPENDENT
// arrays — each owns a distinct buffer, each disposed exactly once (a shared
// buffer would double-free). Looped: t accumulates 2 + 2 per iteration → 4000.
const char* kTwiceLoopLeakFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do "
    "      using var s := CString.From(\"hi\"); "
    "      using var a := s.ToBytes(); "
    "      using var b := s.ToBytes(); "
    "      t := t + a.Count + b.Count; i := i + 1; "
    "    end; "
    "    if t = 4000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// stdlib §10.1: CString.Length / From / FromBytes / disposal remain LSan-clean
// (unaffected by the ToBytes change). FromBytes of [104,105,106] has Length 3,
// From("hi") has Length 2 → 5. Looped to amplify any per-iteration leak.
const char* kLengthLoopLeakFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do "
    "      using var b: Array<byte> := [104, 105, 106]; "
    "      using var s1 := CString.FromBytes(b); "
    "      using var s2 := CString.From(\"hi\"); "
    "      t := t + s1.Length + s2.Length; i := i + 1; "
    "    end; "
    "    if t = 5000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ---- Value correctness (the #1000 round-trip cases) -------------------------

// stdlib §10.1: FromBytes round-trip — back.Count is 2 (was 0/garbage/SIGABRT).
BEZEL_TEST(codegen_cstring_tobytes_1000, frombytes_roundtrip_count_10_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kFromBytesRoundTripCount), 2);
}

// stdlib §10.1: FromBytes round-trip — bytes survive the copy (back[0,1]=104,105).
BEZEL_TEST(codegen_cstring_tobytes_1000, frombytes_roundtrip_values_10_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kFromBytesRoundTripValues), 0);
}

// stdlib §10.1: From(string) round-trip — "hi" → 2 bytes.
BEZEL_TEST(codegen_cstring_tobytes_1000, fromstring_roundtrip_count_10_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kFromStringRoundTripCount), 2);
}

// stdlib §10.1: From(string) round-trip — byte values 104, 105.
BEZEL_TEST(codegen_cstring_tobytes_1000, fromstring_roundtrip_values_10_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kFromStringRoundTripValues), 0);
}

// ---- ASan dispose-exactly-once (no leak, no double-free) --------------------

// stdlib §10.1 / §28.3: round-trip — CString and Array<byte> each disposed once.
BEZEL_TEST(codegen_cstring_tobytes_1000, roundtrip_loop_leak_free_28_3) {
    expectValueAndLeakFree(kRoundTripLoopLeakFree, 0);
}

// stdlib §10.1 / §28.3: the Array<byte> outlives the CString — it owns its buffer
// (no UAF), disposed exactly once.
BEZEL_TEST(codegen_cstring_tobytes_1000, outlive_loop_leak_free_28_3) {
    expectValueAndLeakFree(kOutliveLoopLeakFree, 0);
}

// stdlib §10.1 / §28.3: two ToBytes() → two independent arrays, each freed once.
BEZEL_TEST(codegen_cstring_tobytes_1000, twice_loop_leak_free_28_3) {
    expectValueAndLeakFree(kTwiceLoopLeakFree, 0);
}

// stdlib §10.1: Length / From / FromBytes / disposal remain LSan-clean.
BEZEL_TEST(codegen_cstring_tobytes_1000, length_loop_leak_free_10_1) {
    expectValueAndLeakFree(kLengthLoopLeakFree, 0);
}
