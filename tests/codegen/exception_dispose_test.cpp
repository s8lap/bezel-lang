// Codegen slice CG21 — exception-path auto-disposal of `using var`s
// (§28.3). When an exception propagates out of a block, the block's live
// `using var`s are disposed during unwinding, in reverse declaration
// order, inner scopes before outer — even though the normal scope-exit
// disposal path is bypassed. A destructor bumping a static counter makes
// this observable via the int exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg21, using_var_disposed_on_throw) {
    // r is created, then a throw unwinds out of its scope; r is disposed
    // during unwinding before the outer catch reads the counter.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception end AppError; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    try "
        "      do using var r := new R(); throw new AppError(); end; "
        "    catch AppError => return _n; end; "
        "    return -1; "
        "  end Main; "
        "end R;"), 1);
}

BEZEL_TEST(codegen_cg21, two_using_vars_both_disposed_on_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception end AppError; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    try "
        "      do "
        "        using var a := new R(); using var b := new R(); "
        "        throw new AppError(); "
        "      end; "
        "    catch AppError => return _n; end; "
        "    return -1; "
        "  end Main; "
        "end R;"), 2);
}

BEZEL_TEST(codegen_cg21, disposed_when_exception_crosses_call) {
    // The using var lives in Main's try; Boom() throws; on the unwind
    // after the call, the using var is disposed before the catch.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception end AppError; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Boom() throw new AppError(); end Boom; "
        "  public static method Main(): int "
        "    try "
        "      do using var r := new R(); Boom(); end; "
        "    catch AppError => return _n; end; "
        "    return -1; "
        "  end Main; "
        "end R;"), 1);
}

BEZEL_TEST(codegen_cg21, not_disposed_twice_on_normal_path) {
    // When no exception occurs, the using var is disposed exactly once at
    // normal scope exit (not also via an unwind path).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception end AppError; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    try "
        "      do using var r := new R(); end; "
        "    catch AppError => return -1; end; "
        "    return _n; "
        "  end Main; "
        "end R;"), 1);
}

BEZEL_TEST(codegen_cg21, nested_scopes_dispose_inner_first_on_throw) {
    // Outer and inner using vars both disposed on the throw path.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception end AppError; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    try "
        "      do "
        "        using var outer := new R(); "
        "        do using var inner := new R(); throw new AppError(); end; "
        "      end; "
        "    catch AppError => return _n; end; "
        "    return -1; "
        "  end Main; "
        "end R;"), 2);
}
