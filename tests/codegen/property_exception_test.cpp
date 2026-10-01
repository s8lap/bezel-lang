// Codegen — exceptions thrown inside a property getter or setter must
// propagate to the surrounding try/catch (FINDING [4]).
//
// §31.1/§31.2: a try/catch catches exceptions thrown transitively by any
// callee — and a property accessor invoked by a read/write IS such a callee.
// §23.3: the block-form property's normative example shows a SETTER that
// `throw new ArgumentException(...)` for an invalid value — a sanctioned
// idiom that must be catchable. §17.12: `throw` is valid in any expression
// position, so an expression-bodied accessor may throw too.
//
// The defect had two distinct manifestations, both fixed here:
//
//   1. RESOLUTION GAP: TypeRefs embedded in a property-accessor body (e.g.
//      `new Exception(...)` / `new ArgumentException(...)`) were never resolved
//      by the type-resolution pass (it walked method/ctor/operator bodies but
//      not property bodies), so `new T(...)` had a null resolved type and
//      codegen elided the construction — the `throw` then carried a NULL
//      exception value. The block setter and the expression-body getter both
//      "threw nothing": the catch saw a null in-flight exception, no arm
//      matched, and the exception escaped → process exit 70. The fix resolves
//      accessor bodies exactly as method bodies are resolved.
//
//   2. MISSING POST-CALL PENDING CHECK: a block-form getter (`get => do … end`)
//      is lowered as a STANDALONE function; a throw inside it leaves the
//      pending-exception global set and returns. The read site called it with a
//      plain CreateCall and no post-call pending check (unlike an ordinary
//      method call, which checks via emitPendingCheck) — so the throw was
//      SWALLOWED and the read continued with a garbage value. The fix emits the
//      same post-call pending check the method-call path uses.
//
// The caught cases are observed via the JIT exit code (the int the catch arm
// produces). The no-handler case (an accessor throw that escapes Main) calls
// exit(70), which would terminate the JIT's own process — so it uses the
// native compile+run harness, mirroring uncaught_exit_test.cpp.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

constexpr int kUnhandledExceptionExit = 70;

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Native compile+run, for the exit(70) no-handler path. Mirrors
// uncaught_exit_test.cpp::compileAndRun.
int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_prop_exc");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
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

}  // namespace

// ── Block-form GETTER (the swallow case) ─────────────────────────────────────
// §31.1/§31.2 + §23.3 (FINDING [4]): a block-form getter that bumps a static
// counter then throws, read inside try/catch. The body runs (counter → 1) AND
// the throw is caught (caught → 100). Pre-fix the throw was SWALLOWED: caught
// stayed 0 and the read returned a garbage value → 0 + 1 = 1.
BEZEL_TEST(codegen_property_exception, block_getter_throw_caught) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static calls: int := 0; "
        "  public property Bad: int "
        "    get => do calls := calls + 1; throw new Exception(\"nope\"); end; "
        "  end Bad; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var caught := 0; "
        "    try caught := c.Bad; catch Exception => caught := 100; end; "
        "    return caught + calls; "       // spec: 100 + 1 = 101
        "  end Main; "
        "end C;"), 101);
}

// ── Expression-body GETTER (the escape case) ─────────────────────────────────
// §31.2 + §17.12 (FINDING [4]): an expression-body getter whose body is a
// conditional reaching `throw`, read inside try/catch. Pre-fix the throw's
// `new Exception(...)` was elided (unresolved type) and the null in-flight
// exception escaped Main → exit 70. It must now be caught (caught → 100).
BEZEL_TEST(codegen_property_exception, expr_getter_throw_caught) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static calls: int := 0; "
        "  public property Bad: int "
        "    get => (if calls < 0 then 7 else throw new Exception(\"nope\")); "
        "  end Bad; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var caught := 0; "
        "    try caught := c.Bad; catch Exception => caught := 100; end; "
        "    return caught; "               // spec: 100 (caught)
        "  end Main; "
        "end C;"), 100);
}

// ── Block-form SETTER (the §23.3 normative example) ──────────────────────────
// §23.3 (FINDING [4]): the spec's own block-setter example — `set => do if
// value < 0 then throw new ArgumentException(...) end; _b := value; end` — with
// `c.Balance := -5` inside try/catch. Pre-fix the `new ArgumentException(...)`
// was elided and the null exception escaped Main → exit 70. It must be caught
// (caught → 42).
BEZEL_TEST(codegen_property_exception, block_setter_throw_caught) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _b: int := 0; "
        "  public property Balance: int "
        "    get => _b; "
        "    set => do if value < 0 then throw new ArgumentException(\"neg\"); end; "
        "             _b := value; end; "
        "  end Balance; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var caught := 0; "
        "    try c.Balance := -5; catch ArgumentException => caught := 42; end; "
        "    return caught; "               // spec: 42 (caught)
        "  end Main; "
        "end C;"), 42);
}

// ── Expression-body SETTER ───────────────────────────────────────────────────
// §17.12 + §23.3 (FINDING [4]): an expression-body setter whose body is a
// conditional reaching `throw`, written via `c.Balance := -5` inside try/catch.
// Must be caught (caught → 42), not escape.
BEZEL_TEST(codegen_property_exception, expr_setter_throw_caught) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _b: int := 0; "
        "  public property Balance: int "
        "    get => _b; "
        "    set => (if value >= 0 then _b else throw new ArgumentException(\"neg\")); "
        "  end Balance; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var caught := 0; "
        "    try c.Balance := -5; catch ArgumentException => caught := 42; end; "
        "    return caught; "               // spec: 42 (caught)
        "  end Main; "
        "end C;"), 42);
}

// ── No surrounding handler → propagates out of Main (exit 70) ─────────────────
// §31.2/§33.3 (FINDING [4]): a property accessor whose throw has no enclosing
// handler must propagate to the top-level handler exactly as a method's
// uncaught throw does — the runtime intercepts it and exits non-zero (70). This
// proves the fix routes the accessor throw into the normal unwind machinery,
// not that it merely suppresses the escape. Native harness: exit(70) would kill
// the JIT process.
BEZEL_TEST(codegen_property_exception, block_getter_throw_no_handler_exits_70) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public property Bad: int "
        "    get => do throw new Exception(\"nope\"); end; "
        "  end Bad; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Bad; "               // no try → escapes Main → exit 70
        "  end Main; "
        "end C;"),
        kUnhandledExceptionExit);
}

// ── Propagation across a frame: caught by an OUTER caller ─────────────────────
// §31.2 (FINDING [4]): a block getter throws inside a helper method that has no
// try; the throw must propagate OUT of that method and be caught by the caller's
// try (just like an ordinary method's uncaught throw). caught → 55, plus the
// body ran once (calls → 1) = 56.
BEZEL_TEST(codegen_property_exception, getter_throw_caught_by_outer_caller) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static calls: int := 0; "
        "  public property Bad: int "
        "    get => do calls := calls + 1; throw new Exception(\"nope\"); end; "
        "  end Bad; "
        "  public method ReadBad(): int  return this.Bad; end ReadBad; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var caught := 0; "
        "    try caught := c.ReadBad(); catch Exception => caught := 55; end; "
        "    return caught + calls; "       // 55 + 1 = 56
        "  end Main; "
        "end C;"), 56);
}

// ── Owned-local disposal still runs on the accessor-throw unwind path ─────────
// §28.3 (FINDING [4]): when an accessor throw unwinds out of a block that holds
// a live owned `using var`, that local's destructor must still run on the way
// out. R's destructor bumps a static counter; the helper's `using var c` is
// disposed when its getter throws, and Main's owned `r` is disposed as the
// exception unwinds to the catch. count(1) + caught(5) = 6.
BEZEL_TEST(codegen_property_exception, getter_throw_runs_owned_local_disposal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static count: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R.count := R.count + 1; end destructor; "
        "end R; "
        "class C "
        "  public property Bad: int "
        "    get => do throw new Exception(\"nope\"); end; "
        "  end Bad; "
        "  public static method ReadIt(): int "
        "    using var c := new C(); "
        "    return c.Bad; "
        "  end ReadIt; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try "
        "      do using var r := new R(); caught := ReadIt(); end; "
        "    catch Exception => caught := 5; end; "
        "    return R.count + caught; "     // 1 (disposed) + 5 (caught) = 6
        "  end Main; "
        "end C;"), 6);
}

// ── CONTROLS: normal (non-throwing) property access is unaffected ─────────────

// A block getter + block setter with no throw must read/write correctly.
BEZEL_TEST(codegen_property_exception, control_block_property_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _b: int := 0; "
        "  public property Balance: int "
        "    get => do return _b; end; "
        "    set => do if value < 0 then throw new ArgumentException(\"neg\"); end; "
        "             _b := value; end; "
        "  end Balance; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c.Balance := 33; "
        "    return c.Balance; "            // 33: setter stored, getter read back
        "  end Main; "
        "end C;"), 33);
}

// An expression-body getter with no throw must read correctly.
BEZEL_TEST(codegen_property_exception, control_expr_getter_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _v: int := 7; "
        "  public property Doubled: int => _v * 2; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Doubled; "            // 14
        "  end Main; "
        "end C;"), 14);
}

// CONTROL: an ordinary METHOD throw caught in the identical try/catch still
// works (the path that was always correct must remain correct).
BEZEL_TEST(codegen_property_exception, control_method_throw_caught) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public method Bad(): int  throw new Exception(\"nope\"); end Bad; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var caught := 0; "
        "    try caught := c.Bad(); catch Exception => caught := 42; end; "
        "    return caught; "               // 42
        "  end Main; "
        "end C;"), 42);
}
