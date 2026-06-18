// fix #756 / §27.6: a property reference used in function-value position
// (`var f: () -> int := b.Count`) produces the getter as a function value —
// equivalent to `() => b.Count`. Previously it silently compiled and SIGSEGV'd:
// the getter was evaluated immediately and the int was stored into a
// function-pointer slot, then called.
//
// Fix: sema target-types the property reference to the function (checkAssignable
// promotion at local-decl / assignment / return positions), and codegen emits a
// property thunk (makePropertyThunk) that captures the receiver (a borrow) and
// computes the getter on call — handling computed, block-form, virtual/interface
// (vtable/itable dispatch), and auto-property getters.
//
// NOTE: drives the real Driver (compileAndRun) — runJit does not crash-test
// these reliably. Receiver-lifetime escapes (returning/storing the thunk beyond
// the receiver's scope) are a pre-existing closure-capture gap shared with
// method references — tracked separately, not here.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_756_" + std::to_string(counter++);
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
}  // namespace

// §27.6/#756: the issue's exact repro — computed (arrow) getter. `fn()` returns
// the getter value (7); previously SIGSEGV.
BEZEL_TEST(property_ref_function_value_756, computed_getter_repro) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int => _n; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(7); "
        "  var fn: () -> int := b.Count; "
        "  return fn(); "  // 7
        "end Main; end Program;"), 7);
}

// §27.6: a block-form getter (`get => do … end`) thunked.
BEZEL_TEST(property_ref_function_value_756, block_getter) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int get => do return _n + 1; end; end; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(7); "
        "  var fn: () -> int := b.Count; "
        "  return fn(); "  // 8
        "end Main; end Program;"), 8);
}

// §27.6: a VIRTUAL property reference dispatches DYNAMICALLY through the thunk —
// a base-typed receiver holding a derived instance calls the override's getter.
BEZEL_TEST(property_ref_function_value_756, virtual_property_dispatch) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Base public virtual property V: int => 1; destructor end destructor; end Base; "
        "class Derived: Base public override property V: int => 2; end Derived; "
        "class Program public static method Main(): int "
        "  using var d := new Derived(); "
        "  var bb: Base := d; "
        "  var fn: () -> int := bb.V; "
        "  return fn(); "  // 2 (Derived's getter)
        "end Main; end Program;"), 2);
}

// §27.6: an auto-property getter thunked.
BEZEL_TEST(property_ref_function_value_756, auto_property) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  public property Count: int; "
        "  public constructor(n: int) Count := n; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(6); "
        "  var fn: () -> int := b.Count; "
        "  return fn(); "  // 6
        "end Main; end Program;"), 6);
}

// §27.6 control: an ordinary value-position read is UNAFFECTED — `var n: int :=
// b.Count` still reads the property value (the promotion only fires for a
// function-typed target).
BEZEL_TEST(property_ref_function_value_756, value_position_unaffected) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int => _n; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(5); "
        "  var n: int := b.Count; "
        "  return n; "  // 5
        "end Main; end Program;"), 5);
}

// §27.6: the promotion also fires in ASSIGNMENT position (not just local-decl
// init) — `fn := b.Count` where fn is a function-typed local.
BEZEL_TEST(property_ref_function_value_756, assignment_position) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int => _n; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(8); "
        "  var fn: () -> int := () => 0; "
        "  fn := b.Count; "
        "  return fn(); "  // 8
        "end Main; end Program;"), 8);
}
