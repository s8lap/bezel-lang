// Regression tests for #1596 (§5.1 implicit widening conversions / §29.3
// Yield): a `yield return` whose value's LLVM type is NARROWER than the
// iterator's element type (e.g. a `byte` span element yielded into an
// `IEnumerable<int>`) is a WIDENING conversion, but the yield-return lowering
// at LlvmBackend_Stmts.cpp unconditionally called `CreateTrunc(v,
// iterElemTy_)` whenever the two integer types differed. `CreateTrunc`
// requires DestTy strictly narrower than SrcTy; called the other way around
// (i8 -> i32) it emits malformed IR ("DestTy too big for Trunc") that aborts
// LLVM's IR verifier — a codegen crash on a program sema accepts correctly
// (§5.1: `byte -> int` is a lossless implicit widening).
//
// The fix widens with CreateZExt for unsigned source primitives (byte,
// ushort, uint, ulong, char — Emitter::isUnsignedPrim) and CreateSExt
// otherwise, only falling back to CreateTrunc when the source is genuinely
// the wider type. These tests exercise both the exact issue repro (a
// `ArraySpan<byte>` indexer result yielded as `int`) and the signedness
// choice directly (an unsigned `byte` and a signed `sbyte` widened to `int`,
// each with a value that would print wrong under the other extension mode).
//
// runJit returns the FULL int from Main() (not 8-bit wrapped).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Exact repro from #1596: a byte read out of an `ArraySpan<byte>` via the
// nullable-forcing indexer (`s[0]!`) and yielded from an `IEnumerable<int>`
// iterator. Before the fix this crashed codegen (malformed `trunc i8 to
// i32`) instead of returning a value; 200 is > 127 so a wrong sign-extension
// (treating the byte as signed) would also read back incorrectly (-56).
BEZEL_TEST(iter_yield_widen_byte_1596, span_byte_indexer_yielded_as_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(arr: Array<byte>): IEnumerable<int> "
        "    var s := arr.AsSpan(); "
        "    var n := s[0]!; "
        "    yield return n; "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var arr := new Array<byte>(1, 200); "
        "    var v := 0; "
        "    for var x in C.Gen(arr) do v := x; end; "
        "    return v; "                                    // 200
        "  end Main; end C;"), 200);
}

// A plain `byte` local widened to `int` through yield: zero-extension must
// preserve the unsigned value (200), not sign-extend it to -56.
BEZEL_TEST(iter_yield_widen_byte_1596, byte_local_zero_extends) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var b: byte := 200; "
        "    yield return b; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var v := 0; "
        "    for var x in C.Gen() do v := x; end; "
        "    return v; "                                    // 200, not -56
        "  end Main; end C;"), 200);
}

// A signed `sbyte` local widened to `int` through yield: sign-extension must
// preserve a negative value (-5), not zero-extend it to 251.
BEZEL_TEST(iter_yield_widen_byte_1596, sbyte_local_sign_extends) {
    // -5 does not fit an 8-bit exit code — observe on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var b: sbyte := -5; "
        "    yield return b; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var v := 0; "
        "    for var x in C.Gen() do v := x; end; "
        "    Console.WriteLine(v); return 0; "                // -5, not 251
        "  end Main; end C;"), "-5\n");
}
