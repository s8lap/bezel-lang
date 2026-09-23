// fix #833 / §30.7: an `await for var x in <async enumerable>` (or a plain
// `for var x in <coll>` inside an async method) with an INFERRED loop variable
// whose element type is wider than i32 — a class/string pointer, or a `long` —
// sized the loop variable's async-frame slot as i32. On the per-suspend spill the
// 8-byte value overflowed the 4-byte slot into the adjacent outerTask field,
// corrupting it → SIGSEGV on resume. collectSuspendLocals now derives the
// inferred loop variable's type from the iterable's element (genericArgs[0]).
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Class (ptr) element — the bug01 shape (was SIGSEGV). Accumulate Box.Value.
BEZEL_TEST(async_iter_var_frame_width_833, class_element_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public Value: int; "
        "  public constructor(v: int) Value := v; end; "
        "  destructor end destructor; end Box; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<Box> "
        "    yield return new Box(4); yield return new Box(5); end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x.Value; end; "
        "    return sum; "                 // 9
        "  end Main; end C;"), 9);
}

// String (ptr) element.
BEZEL_TEST(async_iter_var_frame_width_833, string_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<string> "
        "    yield return \"hi\"; yield return \"abc\"; end Gen; "
        "  public static async method Main(): Task<int> "
        "    var n := 0; "
        "    await for var s in Gen() do n := n + s.Length; end; "
        "    return n; "                   // 5
        "  end Main; end C;"), 5);
}

// `long` element (i64, wider than i32) — also overflowed the i32 slot.
BEZEL_TEST(async_iter_var_frame_width_833, long_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<long> "
        "    yield return 5000000000L; end Gen; "
        "  public static async method Main(): Task<int> "
        "    var found := 0; "
        "    await for var v in Gen() do "
        "      if v = 5000000000L then found := 1; end; end; "
        "    return found; "               // 1
        "  end Main; end C;"), 1);
}

// Regression: a range loop (i32 loop variable) inside an async method still
// works — the i32 default is kept (a RangeExpr iterable has no genericArgs).
BEZEL_TEST(async_iter_var_frame_width_833, range_loop_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; for var i in 1..3 do sum := sum + i; end; return sum; "
        "  end Main; end C;"), 6);
}
