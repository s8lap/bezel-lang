// fix #791 / §28.3 / §28.9 / §27.6: a method or property REFERENCE (`recv.M`,
// used as a function value) captures the receiver as a BORROW in its closure
// cell — `b.GetN` ≡ `() => b.GetN()`. When that reference ESCAPES the receiver's
// scope (returned, or stored into a longer-lived field), the borrow outlives its
// source and the deferred call reads freed memory. The ownership/borrow checker
// did not model the member-reference receiver borrow (it is not a LambdaExpr), so
// it silently compiled and UAF'd. Now `returnedExprMayBorrow` recognizes a
// method/property reference as borrow-bearing, so the existing §28.3 borrow-escape
// checks reject the escaping case.
//
// The in-scope use (`var f := b.M; f();` within b's scope) is correct and must
// still compile and run. These are ownership accept/reject tests, so they drive
// the real Driver (runJit does not run the ownership pass).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir791() {
    return wtScratchDir("wt-791");
}

int compileAndRun(const std::string& source) {
    auto dir = wtDir791();
    std::string stem = unique_stem("bezel_791");
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

bool compileFails(const std::string& source) {
    auto dir = wtDir791();
    std::string stem = unique_stem("bezel_791_neg");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}
}  // namespace

// §28.3/§28.9: the issue's exact repro — a METHOD reference to a `using var`
// local's method, returned, escapes the local's scope. Must be rejected.
BEZEL_TEST(escaping_member_ref_borrow_791, method_ref_return_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class Box public method GetN(): int return 4; end GetN; end Box; "
        "class P "
        "  public static method GetM(): () -> int "
        "    using var b := new Box(); "
        "    return b.GetN; "            // borrow of b escapes
        "  end GetM; "
        "  public static method Main(): int => GetM()(); "
        "end P;"));
}

// §27.6: a PROPERTY reference (promoted to a function value) to a `using var`
// local's property, returned, also escapes. Must be rejected.
BEZEL_TEST(escaping_member_ref_borrow_791, property_ref_return_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class Box public property Count: int => 9; end Box; "
        "class P "
        "  public static method GetM(): () -> int "
        "    using var b := new Box(); "
        "    return b.Count; "           // borrow of b escapes
        "  end GetM; "
        "  public static method Main(): int => GetM()(); "
        "end P;"));
}

// §28.9: storing a method reference of a scope-bound local into a longer-lived
// field is the indirect escape — also rejected.
BEZEL_TEST(escaping_member_ref_borrow_791, method_ref_store_to_field_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class Box public method GetN(): int return 5; end GetN; end Box; "
        "class Holder "
        "  public f: () -> int; "
        "  public constructor() f := lambda() => 0; end constructor; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    using var b := new Box(); "
        "    h.f := b.GetN; "           // borrow of b stored into h.f
        "    return 0; "
        "  end Main; "
        "end P;"));
}

// §27.6 control: using a method reference WITHIN the receiver's scope is correct
// — it must still compile and run (no over-rejection). f() returns 7.
BEZEL_TEST(escaping_member_ref_borrow_791, in_scope_method_ref_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box public method GetN(): int return 7; end GetN; end Box; "
        "class P "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var f: () -> int := b.GetN; "
        "    return f(); "               // 7 — in-scope, correct
        "  end Main; "
        "end P;"), 7);
}

// §27.6 control: a method reference whose receiver OUTLIVES the closure (a method
// parameter, not a scope-bound local) is not an escape — must compile.
BEZEL_TEST(escaping_member_ref_borrow_791, param_receiver_ref_return_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  public _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method GetN(): int return _v; end GetN; "
        "end Box; "
        "class P "
        "  public static method Pick(b: Box): () -> int => b.GetN; "
        "  public static method Main(): int "
        "    using var box := new Box(8); "
        "    var f := Pick(box); "       // receiver is a param (caller-owned); ok
        "    return f(); "               // 8
        "  end Main; "
        "end P;"), 8);
}
