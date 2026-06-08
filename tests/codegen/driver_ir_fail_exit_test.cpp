// Driver robustness — IR verification failure must propagate as a non-zero
// exit code (#506).
//
// §2 (Compilation pipeline): the compiler must not silently succeed when the
// LLVM IR verifier reports malformed IR. Before the fix, Emitter::run()
// printed the verifier's report to stderr but still returned the module to
// the caller, so the Driver received a non-null module, emitted an object file
// from it, linked a binary, and returned CompileResult{success=true} — exit 0.
//
// This file tests the driver-level invariant:
//   1. An IR-failing program → CompileResult::success == false, no output file.
//   2. A valid program (control) → CompileResult::success == true, runnable binary.
//
// The IR-failing program used here is a generic method called with a CLASS type
// argument (issue #505), which currently emits "Function return type does not
// match operand type of return inst" because codegen substitutes the concrete
// class type in the return position but emits a `ret void` instead of `ret <T>`.
// That specific miscompile may be fixed later; if so, a different reliably-IR-
// failing program can be substituted for the NEGATIVE test (see comment there).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Write `source` to a temp .bzl file and compile it to `outExe`. Returns the
// CompileResult. Cleans up the source file; the caller must remove outExe if
// it was created.
bezel::CompileResult compileToExe(const std::string& source,
                                   const std::string& outExe) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string src =
        (dir / ("bezel_506_" + std::to_string(counter++) + ".bzl")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, outExe);
    std::remove(src.c_str());
    return r;
}

} // namespace

// ── Negative: IR-failing program → failure status, no output binary ──────────

// #506 / §2: A generic method invoked with a class type argument triggers a
// codegen defect (issue #505) that produces malformed IR ("Function return type
// does not match operand type of return inst"). Before the fix, bezelc returned
// success=true and wrote (or linked) a broken binary. After the fix it must
// return success=false and must NOT create the output file.
//
// NOTE: If issue #505 is fixed in the future and this specific program no longer
// produces malformed IR, replace it with any other reliably-IR-failing program
// to keep the negative test meaningful.
BEZEL_TEST(codegen_driver_ir_fail_exit, ir_fail_returns_failure_status) {
    auto dir = fs::temp_directory_path();
    std::string exePath =
        (dir / "bezel_506_ir_fail_control_out").string();

    // Remove any stale output from a prior run.
    std::remove(exePath.c_str());

    // Generic method called with a class type → malformed IR (issue #505).
    bezel::CompileResult r = compileToExe(
        "module M; "
        "class A "
        "  public v: int; "
        "  public constructor() v := 7; end constructor; "
        "  destructor end destructor; "
        "end A; "
        "class Util "
        "  public static method GetV<T>(obj: T, getter: (T) -> int): int "
        "    => getter(obj); "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new A(); "
        "    return Util.GetV(a, (x) => x.v); "
        "  end Main; "
        "end C;",
        exePath);

    // The result must be failure.
    BEZEL_EXPECT_FALSE(r.success);

    // The output binary must NOT exist (pipeline must have been stopped before
    // the linker ran and created the file).
    bool exists = fs::exists(exePath);
    if (exists) std::remove(exePath.c_str());  // clean up even on failure
    BEZEL_EXPECT_FALSE(exists);
}

// ── Control: a valid program still compiles successfully ─────────────────────

// Ensure the fix does not regress clean programs. A correct program must still
// produce CompileResult::success == true.
BEZEL_TEST(codegen_driver_ir_fail_exit, clean_program_still_succeeds) {
    auto dir = fs::temp_directory_path();
    std::string exePath =
        (dir / "bezel_506_clean_out").string();

    std::remove(exePath.c_str());

    bezel::CompileResult r = compileToExe(
        "module M; class C public static method Main(): int => 42; end C;",
        exePath);

    BEZEL_EXPECT_TRUE(r.success);

    // Clean up the produced binary.
    std::remove(exePath.c_str());
}
