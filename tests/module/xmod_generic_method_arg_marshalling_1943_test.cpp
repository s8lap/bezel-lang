// Cross-module imported GENERIC METHOD called with a NON-i32-width type ARGUMENT
// (#1943 / #428 family), §14.1 monomorphization, §2.5 modules, §2.8.3 rule 2
// "external instantiation behaves identically". This is the ARGUMENT-side
// sibling of the return-side #1937 gate.
//
//   module Lib;
//   public class Util
//     public static method Count<T>(x: T): int where T: value return 7;
//   end Util;
//
//   // importer:  Lib.Util.Count<int>(3)     -->  OK (returns 7)
//   //            Lib.Util.Count<double>(3.0) -->  IR VERIFICATION FAILURE
//   //            Lib.Util.Count<long>(y)     -->  IR VERIFICATION FAILURE
//
// Root cause: unlike a generic CLASS's methods (#783, exported as methodBodyText
// and reconstructed + monomorphized importer-side), a generic METHOD on a
// non-generic class has NO reconstructed body. In the defining module its body
// is compiled against the i32 placeholder, so methodFns_ holds only the external
// i32-PLACEHOLDER template `bz$Util$Count(i32)`. When the importer calls it at a
// type whose ABI is not that i32 placeholder — long (i64), double, bool (i1),
// float (f32), a record struct — a type-param-typed argument reaches the
// placeholder parameter at a DIFFERENT LLVM width, so codegen emits
//   call i32 @bz$Util$Count(i64 %y)      ; or (double ...), (i1 ...), ...
// which LLVM's module verifier rejects with "Call parameter type does not match
// function signature" — a raw, unlocated abort with no clean diagnostic.
//
// The importer-side generic-method monomorphization that would make the call
// CORRECT is the #428 core (the same blocker as #1937). Until it lands, the
// shape is FAIL-CLOSED at codegen (LlvmBackend_Calls2.cpp, emitMethodCall): the
// call is rejected with a clean, LOCATED §14.1/§2.8.3/#1943 diagnostic instead
// of the verifier abort. A clean, source-located compile error is strictly
// better UX than an ICE-like verifier report.
//
// FAIL-WITHOUT / PASS-WITH HINGE. #1943 already fails LOUDLY (the compile fails
// both with and without the fix), so `!success` alone is NOT a hinge. The
// difference is the DIAGNOSTIC: WITHOUT the fix the driver's stderr carries only
// the raw "Call parameter type does not match function signature" verifier text
// and NO `#1943` marker; WITH the fix the reported Error makes Emitter::run()
// return nullptr and the driver prints the clean `#1943` diagnostic. So T1/T2
// capture the driver's stderr and assert it contains `#1943` — false (raw abort
// only) without the fix, true with it.
//
// TEST MATRIX:
//   T1  Count<double>(3.0): rejected with a clean #1943 diagnostic
//       (was: raw verifier abort "Call parameter type does not match ...").
//   T2  Count<long>(y) with y a genuine i64 local: rejected with #1943
//       (a literal `99` would narrow to i32 and NOT trip the gate — see T5 — so
//       T2 forces a real i64 argument through a variable).
//   T3  CONTROL (must still COMPILE & RUN, returns 7): Count<int>(3) — the arg
//       is i32, matches the placeholder, no mismatch, gate does not fire.
//   T4  CONTROL (must still COMPILE & RUN, returns 5): an imported NON-generic
//       method taking `long` (Plain(x: long): int) called with a long — the
//       gate is scoped to generic methods (non-empty genericParams), so a
//       non-generic imported method with a wide argument is untouched.
//   T5  CONTROL (must still COMPILE & RUN, returns 7): Count<long>(99) with a
//       LITERAL argument — the int literal narrows to i32 at the call, so arg
//       and placeholder param types are EQUAL and the gate does not fire. This
//       pins that the gate is precise (fires on the actual LLVM-type mismatch,
//       not on the `<long>` type argument) and does not over-reject the
//       #1937-T5 shape.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile1943 {
    std::string path;
    explicit TempFile1943(std::string p) : path(std::move(p)) {}
    ~TempFile1943() { std::remove(path.c_str()); }
};

void writeSource1943(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile A → object. Returns true on success.
bool compileToObj1943(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A → exe, run, return exit code (-1 on compile/link/run
// failure). Used by the positive-control (must-run) cases.
int compileImportAndRun1943(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`.
// Returns the compile's success flag (expected false for the gated shapes).
// The stderr capture is the fail-without/pass-with hinge: only WITH the #1943
// fix does the driver emit the clean `#1943` diagnostic (WITHOUT it, stderr
// carries only the raw verifier abort). fd 2 is redirected so BOTH the driver's
// std::cerr diagnostics AND LLVM's llvm::errs() verifier text are captured.
bool compileImportCaptureErr(const std::string& bSrc, const std::string& bExe,
                             const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz1943_err_" + std::to_string(std::rand()) + ".txt"))
            .string();

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = driver.compileFile(bSrc, bExe, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

// Library: a generic Count<T> (register-return int, T used in a value parameter)
// and a NON-generic Plain(long) used by the T4 control.
const char* kLib1943 =
    "module Lib1943; "
    "public class Util "
    "  public static method Count<T>(x: T): int "
    "    where T: value "
    "    return 7; "
    "  end Count; "
    "  public static method Plain(x: long): int "
    "    return 5; "
    "  end Plain; "
    "end Util;";

}  // namespace

// ===========================================================================
// T1 — Count<double>(3.0): the cross-module non-i32-width argument call is
// REJECTED with a clean #1943 diagnostic (was: raw verifier abort).
// ===========================================================================
BEZEL_TEST(xmod_generic_method_arg_marshalling_1943,
           t1_double_arg_clean_diagnostic_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz1943_t1_" + std::to_string(std::rand());
    TempFile1943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1943 aObj((dir / (stem + "_a.o")).string());
    writeSource1943(aSrc.path, kLib1943);
    BEZEL_EXPECT_TRUE(compileToObj1943(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App1943D; import Lib1943; "
        "public class C "
        "  public static method Main(): int "
        "    return Lib1943.Util.Count<double>(3.0); "
        "  end Main; "
        "end C;";
    TempFile1943 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1943 bExe((dir / (stem + "_b")).string());
    writeSource1943(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr(bSrcFile.path, bExe.path, aObj.path, err);
    // Fails loudly (true both with and without the fix).
    BEZEL_EXPECT_TRUE(!success);
    // The hinge: only WITH the fix does the clean #1943 diagnostic appear.
    BEZEL_EXPECT_TRUE(err.find("#1943") != std::string::npos);
}

// ===========================================================================
// T2 — Count<long>(y) with y a genuine i64 local: REJECTED with #1943.
// (A literal argument would narrow to i32 and NOT trip the gate — see T5 — so
// the argument here is forced through a `long` variable.)
// ===========================================================================
BEZEL_TEST(xmod_generic_method_arg_marshalling_1943,
           t2_long_var_arg_clean_diagnostic_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz1943_t2_" + std::to_string(std::rand());
    TempFile1943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1943 aObj((dir / (stem + "_a.o")).string());
    writeSource1943(aSrc.path, kLib1943);
    BEZEL_EXPECT_TRUE(compileToObj1943(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App1943L; import Lib1943; "
        "public class C "
        "  public static method Main(): int "
        "    var y: long := 99; "
        "    return Lib1943.Util.Count<long>(y); "
        "  end Main; "
        "end C;";
    TempFile1943 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1943 bExe((dir / (stem + "_b")).string());
    writeSource1943(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("#1943") != std::string::npos);
}

// ===========================================================================
// T3 — CONTROL: Count<int>(3) still COMPILES & RUNS (arg i32 matches the
// placeholder, gate does not fire). Returns 7.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_arg_marshalling_1943,
           t3_int_arg_still_works_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz1943_t3_" + std::to_string(std::rand());
    TempFile1943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1943 aObj((dir / (stem + "_a.o")).string());
    writeSource1943(aSrc.path, kLib1943);
    BEZEL_EXPECT_TRUE(compileToObj1943(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App1943I; import Lib1943; "
        "public class C "
        "  public static method Main(): int "
        "    return Lib1943.Util.Count<int>(3); "  // 7
        "  end Main; "
        "end C;";
    TempFile1943 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1943 bExe((dir / (stem + "_b")).string());
    writeSource1943(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(
        compileImportAndRun1943(bSrcFile.path, bExe.path, aObj.path), 7);
}

// ===========================================================================
// T4 — CONTROL: an imported NON-generic method taking `long` still COMPILES &
// RUNS (the gate is scoped to generic methods). Returns 5.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_arg_marshalling_1943,
           t4_nongeneric_long_arg_still_works_2_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz1943_t4_" + std::to_string(std::rand());
    TempFile1943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1943 aObj((dir / (stem + "_a.o")).string());
    writeSource1943(aSrc.path, kLib1943);
    BEZEL_EXPECT_TRUE(compileToObj1943(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App1943P; import Lib1943; "
        "public class C "
        "  public static method Main(): int "
        "    var y: long := 42; "
        "    return Lib1943.Util.Plain(y); "  // 5
        "  end Main; "
        "end C;";
    TempFile1943 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1943 bExe((dir / (stem + "_b")).string());
    writeSource1943(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(
        compileImportAndRun1943(bSrcFile.path, bExe.path, aObj.path), 5);
}

// ===========================================================================
// T5 — CONTROL: Count<long>(99) with a LITERAL argument still COMPILES & RUNS.
// The int literal narrows to i32 at the call, so the argument matches the
// placeholder param and the gate does not fire — pinning that the gate keys on
// the actual LLVM-type mismatch, not on the `<long>` type argument, and does not
// over-reject the #1937-T5 shape. Returns 7.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_arg_marshalling_1943,
           t5_long_literal_arg_still_works_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz1943_t5_" + std::to_string(std::rand());
    TempFile1943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1943 aObj((dir / (stem + "_a.o")).string());
    writeSource1943(aSrc.path, kLib1943);
    BEZEL_EXPECT_TRUE(compileToObj1943(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App1943Lit; import Lib1943; "
        "public class C "
        "  public static method Main(): int "
        "    return Lib1943.Util.Count<long>(99); "  // 7
        "  end Main; "
        "end C;";
    TempFile1943 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1943 bExe((dir / (stem + "_b")).string());
    writeSource1943(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(
        compileImportAndRun1943(bSrcFile.path, bExe.path, aObj.path), 7);
}

}  // namespace bezel::module
