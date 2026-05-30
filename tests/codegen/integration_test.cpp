// Full-pipeline integration tests: drive the real compiler (front-end
// incl. ownership → LLVM object → clang link), run the produced native
// binary, and check its exit code. Unlike the JIT feature tests (which
// skip ownership/definite-assignment), these exercise the entire pipeline
// on ownership-clean programs, so they validate that real binaries of the
// headline features actually build and run.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace {

int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_integ_" + std::to_string(counter++);
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

} // namespace

BEZEL_TEST(integration, virtual_dispatch_through_base_param) {
    // A Base-typed parameter holding a Square dispatches to the override.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Shape public virtual method Area(): int => 0; end Shape; "
        "class Square : Shape public override method Area(): int => 16; "
        "end Square; "
        "class C "
        "  public static method AreaOf(s: Shape): int => s.Area(); "
        "  public static method Main(): int "
        "    using var sq := new Square(); return AreaOf(sq); "
        "  end Main; "
        "end C;"), 16);
}

BEZEL_TEST(integration, cross_method_exception_propagation) {
    // A callee throws; the caller's try catches it after the call unwinds.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class AppError end AppError; class C "
        "  public static method Risky(n: int): int "
        "    if n < 0 then throw new AppError(); end; return n * 2; "
        "  end Risky; "
        "  public static method Main(): int "
        "    try return Risky(0 - 1); catch AppError => return 99; end; "
        "  end Main; "
        "end C;"), 99);
}

BEZEL_TEST(integration, overflow_checked_and_caught) {
    // A real binary raises and catches OverflowException.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var a := 2000000000; "
        "  try var r := a + a; return r; "
        "  catch OverflowException => return 7; end; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(integration, disposal_via_destructor_counter) {
    // A using var's destructor runs at scope end (a do-block), bumping a
    // static the program then returns.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    do using var r := new R(); end; "
        "    return _n; "
        "  end Main; "
        "end R;"), 1);
}

BEZEL_TEST(integration, case_expression) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var x := 2; "
        "  var r := case x 1 => 1, 2 => 22, _ => 0 end; "
        "  return r; "
        "end Main; end C;"), 22);
}
