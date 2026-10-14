// fix #6278 — spec §30.14 (v0.338): Domain.Spawn's entry must be capture-free.
//
// ExprTC::isCaptureFreeEntry walks a lambda body with exprCaptures, which did
// not descend into InterpolatedStringExpr parts. A capture written only inside
// `$"..."` therefore went unseen, and `Domain.Spawn(lambda() => ...
// Console.WriteLine($"c={n}"))` compiled while `Console.WriteLine(n)` in the
// same position was rejected. The fix makes the walk visit each interpolation
// part's embedded expression, so both spellings take the same rejection.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The capture-free rejection text (the Domain.Spawn check that calls
// isCaptureFreeEntry in ExprTypecheck_Calls.cpp), pinned exactly as the plain
// `Console.WriteLine(n)` form reports it.
const char* kCaptureFreeNeedle =
    "Domain.Spawn entry must be capture-free: a non-capturing lambda or a "
    "static method reference";

bool rejectedCaptureFree(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kCaptureFreeNeedle) != std::string::npos &&
            d.message.find("(§30.14, #600)") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---- rejections: the capture lives only inside the interpolation ----------

BEZEL_TEST(spawn_capture_in_interpolation_6278,
           interpolated_parameter_capture_rejected_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var u := Domain.Spawn(lambda() => do "
        "Console.WriteLine($\"c={args.Count}\"); end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedCaptureFree(r));
}

BEZEL_TEST(spawn_capture_in_interpolation_6278,
           interpolated_local_capture_rejected_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var n := 5; "
        "    using var u := Domain.Spawn(lambda() => do "
        "Console.WriteLine($\"c={n}\"); end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedCaptureFree(r));
}

// Parity: the non-interpolated spelling is rejected with the same text. This
// row is the reference the interpolated rows are measured against.
BEZEL_TEST(spawn_capture_in_interpolation_6278,
           plain_parameter_capture_rejected_parity_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var u := Domain.Spawn(lambda() => do "
        "Console.WriteLine(args.Count); end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedCaptureFree(r));
}

// ---- accept controls: no capture, so the interpolation stays admitted -----

BEZEL_TEST(spawn_capture_in_interpolation_6278,
           capture_free_interpolation_accepted_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var u := Domain.Spawn(lambda() => do "
        "Console.WriteLine($\"c={1+2}\"); end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("c=3") != std::string::npos);
    BEZEL_EXPECT_FALSE(rejectedCaptureFree(r));
}

// The interpolation uses only the lambda's own parameter, passed at the call.
BEZEL_TEST(spawn_capture_in_interpolation_6278,
           own_parameter_interpolation_accepted_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var u := Domain.Spawn(lambda(x: int) => do "
        "Console.WriteLine($\"c={x}\"); end, 7); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("c=7") != std::string::npos);
    BEZEL_EXPECT_FALSE(rejectedCaptureFree(r));
}
