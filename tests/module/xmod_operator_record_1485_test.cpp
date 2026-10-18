// Cross-module imported user-defined operator overload returning a VALUE type
// (record) — issue #1485 (§25 operator overloading, §9 records, §2.5/§2.6
// build phases / import).
//
// Bug #1485: an imported `public static method operator +(a: Box, b: Box): Box`
//   is miscompiled in the IMPORTER when its record-by-value result is consumed.
//   Root cause was sema, not codegen: in ExprTypecheck_Exprs.cpp typeBinary()
//   the BinaryExpr's result type is set only inside a
//   `dynamic_cast<ast::OperatorDecl*>(opm->decl)` branch, which yields null for
//   an imported operator (imported symbols carry no AST node — decl == nullptr).
//   With no sema type on the BinaryExpr, `var r := a + b` infers null, the
//   downstream `r.V` member access fails to resolve, and codegen silently drops
//   the whole expression (fieldAddr() → null) or reads garbage. The fix mirrors
//   the imported-regular-method fallback: when the operator is imported, take
//   the loader-stashed importedReturnType(opm). typeUnary() had the same gap.
//
// The IDENTICAL program with the record + operator declared in the SAME module
// compiles correctly (the CONTROL below), and an imported NON-operator static
// method returning the same record was already fine — so only the imported
// operator path was defective.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"


// ── PORT NOTE (#6386, round 2) ───────────────────────────────────────────────
//
// Every row below was hand-rolled: its own `TempFile` / `writeSource`, its own
// `Dependency` vector, its own `popen` + `WEXITSTATUS` decode. The four
// cross-module rows now call `test::runAotModules`; the same-module control
// stays on `test::runAot`, because #6379 is explicit that `runAotModules` is NOT
// a replacement for `runAot` — a one-module row keeps the content-addressed
// link cache a whole gate relies on.
//
// Three things the port changes, all of them the point of #6386:
//
//   * THE COMPILE HALF IS FORK-ISOLATED. The old rows called
//     `Driver::compileObject` / `compileFile` IN THE TEST PROCESS;
//     `runAotModules` goes through `forkedCompileModule` /
//     `forkedCompileWithDeps`, the same forked step `runAot` uses.
//   * A CRASH NO LONGER READS AS A VALUE. The old `runCapture` returned `-1` for
//     a failed popen, a signal AND a normal exit alike, and every failing
//     `BEZEL_EXPECT_TRUE(ra.success)` was followed by `if (!ra.success) return;`
//     — an ABSORBING GUARD that skipped the `exitCode` and `stdoutText`
//     assertions on that leg. Each row now asserts `r.ran` FIRST and
//     unconditionally and then runs every value assertion. No assertion was
//     removed; the two guards become unreachable by construction because `ran`
//     is false in every case they caught.
//   * SCRATCH MOVED OFF THE SHARED PARENT. `wtDir1485()` + `unique_stem` are
//     retired with the runners; `runAotModules` uses a per-call `mkdtemp` under
//     the shared `scratchBase()`, which is the #3002-class flake fix the hand-rolled
//     dir was working around, now structural.
//
// `r.stdoutText` is the program's stdout with stderr kept separate, exactly the
// channel the old `popen(exe + " 2>/dev/null")` produced. Every expected value
// here is below 256, so no row is exposed to 8-bit exit-code truncation.
//
// TEST NAMES ARE UNCHANGED — a rename is a pin move, not a port (#6386 rule 5).
// No row was added: each ported row keeps its two value assertions verbatim.
//
// Spec: §25, §9, §2.6, §2.5, v0.338.

namespace bezel::module {
namespace {

// ─── §25 / §9 — CONTROL: same-module operator returning a record ─────────────

// The baseline that must not regress: a SAME-MODULE `operator +(Box,Box): Box`
// consumed via `r.V` prints 15 and returns 15.
BEZEL_TEST(xmod_operator_record_1485, same_module_operator_record_control_spec_25) {
    auto r = test::runAot(
        "module App; "
        "record Box(V: int) "
        "  public static method operator +(a: Box, b: Box): Box "
        "    => new Box(a.V + b.V); "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var r := new Box(10) + new Box(5); "
        "    Console.WriteLine(r.V); "
        "    return r.V mod 256; "
        "  end Main; "
        "end Program;");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("15\n"));
}

// ─── §25 / §9 / §2.6 — Bug #1485: imported binary operator returning a record ─

// The core repro. An imported `operator +(Box,Box): Box`, its result consumed
// via `r.V` (direct field access into WriteLine) and via `var v := r.V`, must
// print 15 twice and return 15. Before the fix: the `Console.WriteLine(r.V)`
// line was silently dropped and `r.V` read 0.
BEZEL_TEST(xmod_operator_record_1485, imported_binary_operator_record_spec_25_9) {
    auto r = test::runAotModules({
        {"Lib",
         "module Lib; "
         "public record Box(V: int) "
         "  public static method operator +(a: Box, b: Box): Box "
         "    => new Box(a.V + b.V); "
         "end Box;"},
        {"App",
         "module App; "
         "import Lib; "
         "class Program "
         "  public static method Main(): int "
         "    var r := new Box(10) + new Box(5); "
         "    Console.WriteLine(\"A\"); "
         "    Console.WriteLine(r.V); "         // was silently dropped
         "    Console.WriteLine(\"B\"); "
         "    var v: int := r.V; "              // was read as 0
         "    Console.WriteLine(v); "
         "    Console.WriteLine(\"C\"); "
         "    return r.V mod 256; "             // was 0
         "  end Main; "
         "end Program;"},
    }, "App");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\n15\nB\n15\nC\n"));
}

// ─── §25 / §9 / §2.6 — Bug #1485: imported operator on a MULTI-FIELD record ───

// A multi-field record widened the symptom (uninitialized stack garbage, not
// just 0). Both fields of the imported operator result must materialize.
BEZEL_TEST(xmod_operator_record_1485, imported_operator_multifield_record_spec_25_9) {
    auto r = test::runAotModules({
        {"Lib",
         "module Lib; "
         "public record Vec2(X: int, Y: int) "
         "  public static method operator +(a: Vec2, b: Vec2): Vec2 "
         "    => new Vec2(a.X + b.X, a.Y + b.Y); "
         "end Vec2;"},
        {"App",
         "module App; "
         "import Lib; "
         "class Program "
         "  public static method Main(): int "
         "    var r := new Vec2(10, 3) + new Vec2(5, 4); "  // (15, 7)
         "    Console.WriteLine(r.X); "
         "    Console.WriteLine(r.Y); "
         "    return r.X + r.Y; "                            // 22
         "  end Main; "
         "end Program;"},
    }, "App");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 22);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("15\n7\n"));
}

// ─── §25 / §9 / §2.6 — Bug #1485: imported UNARY operator returning a record ──

// typeUnary() had the identical gap; an imported unary `operator -` returning a
// record must materialize its result.
BEZEL_TEST(xmod_operator_record_1485, imported_unary_operator_record_spec_25_9) {
    auto r = test::runAotModules({
        {"Lib",
         "module Lib; "
         "public record Box(V: int) "
         "  public static method operator -(a: Box): Box "
         "    => new Box(0 - a.V); "
         "end Box;"},
        {"App",
         "module App; "
         "import Lib; "
         "class Program "
         "  public static method Main(): int "
         "    var r := -new Box(15); "  // Box(-15)
         "    Console.WriteLine(r.V); "  // -15
         "    return (0 - r.V) mod 256; " // 15
         "  end Main; "
         "end Program;"},
    }, "App");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-15\n"));
}

// ─── §25 / §2.6 — CONTROL: imported operator returning a PRIMITIVE ───────────

// The narrowing control from the issue: an imported operator returning a
// primitive `int` was already correct and must stay correct (the fix is scoped
// to the value-return path but touches all imported operators).
BEZEL_TEST(xmod_operator_record_1485, imported_operator_primitive_control_spec_25) {
    auto r = test::runAotModules({
        {"Lib",
         "module Lib; "
         "public record Vec2(X: int, Y: int) "
         "  public static method operator *(a: Vec2, b: Vec2): int "
         "    => a.X * b.X + a.Y * b.Y; "
         "end Vec2;"},
        {"App",
         "module App; "
         "import Lib; "
         "class Program "
         "  public static method Main(): int "
         "    var d := new Vec2(2, 3) * new Vec2(4, 5); "  // 8 + 15 = 23
         "    Console.WriteLine(d); "
         "    return d mod 256; "
         "  end Main; "
         "end Program;"},
    }, "App");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 23);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("23\n"));
}

}  // namespace
}  // namespace bezel::module
