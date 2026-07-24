// Regression tests for issue #2193: a closure capture of a case-arm
// payload binding must reclaim the boxed capture cell, whether the closure is
// invoked in the arm or returned. Ordinary local, record, and choice captures
// remain controls. Per §27.5 and §28.9 (v0.226), function values are
// runtime-managed and their capture cells must be reclaimed exactly once; the
// case binding itself is a §19.3/§35.5 borrow scoped to the matching arm.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

void expectClean(const char* source, int expected) {
    JitResult r = runAotChecked(source);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, expected);
}
}  // namespace

// §19.3 / §27.5 / §28.9: the minimal locally-invoked case-arm payload capture.
// The lambda is released before the arm scope ends; its boxed `x` cell must not
// remain as a 4-byte LSan leak.
BEZEL_TEST(case_arm_lambda_capture_2193,
           local_invoke_payload_binding_lsan_clean_spec_19_3) {
    expectClean(
        "module M; choice VBox Some(n: int), None; end VBox; "
        "class C "
        "  public static method Main(): int "
        "    var b := VBox.Some(42); "
        "    var res := 0; "
        "    case b of "
        "      Some(var x) => do var g := lambda(): int => x; res := g(); end, "
        "      None => do end "
        "    end; "
        "    return res; "
        "  end Main; "
        "end C;",
        42);
}

// §19.3 / §27.5 / §28.9: a returned closure owns the environment after the
// matching arm exits. The closure's envdtor must free the sole capture cell at
// last drop, without a second free at the defining method's scope exit.
BEZEL_TEST(case_arm_lambda_capture_2193,
           returned_payload_binding_lsan_clean_spec_19_3) {
    expectClean(
        "module M; choice VBox Some(n: int), None; end VBox; "
        "class C "
        "  public static method Make(): () -> int "
        "    var b := VBox.Some(42); "
        "    case b of "
        "      Some(var x) => do return lambda(): int => x; end, "
        "      None => do return lambda(): int => 0; end "
        "    end; "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(); "
        "    return f(); "
        "  end Main; "
        "end C;",
        42);
}

// §27.5 / §28.9 control: an ordinary value local capture remains clean and is
// not accidentally routed through case-binding cleanup.
BEZEL_TEST(case_arm_lambda_capture_2193,
           ordinary_local_capture_control_lsan_clean_spec_27_5) {
    expectClean(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 42; "
        "    var f := lambda(): int => x; "
        "    return f(); "
        "  end Main; "
        "end C;",
        42);
}

// §9.4 / §27.5 / §28.9 control: a record value local captured by a lambda is
// still an ordinary value capture; it must not acquire cyclic or duplicate
// cleanup through the case-arm path.
BEZEL_TEST(case_arm_lambda_capture_2193,
           record_local_capture_control_lsan_clean_spec_27_5) {
    expectClean(
        "module M; record Pair(X: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pair(42); "
        "    var f := lambda(): int => p.X; "
        "    return f(); "
        "  end Main; "
        "end C;",
        42);
}

// §35.4 / §35.5 / §27.5 / §28.9 control: a value-like choice LOCAL capture is
// distinct from a payload binding capture and remains LSan-clean.
BEZEL_TEST(case_arm_lambda_capture_2193,
           choice_local_capture_control_lsan_clean_spec_35_5) {
    expectClean(
        "module M; choice VBox Some(n: int), None; end VBox; "
        "class C "
        "  public static method Read(v: VBox): int "
        "    var acc := 0; "
        "    case v of Some(var y) => do acc := y; end, None => do end end; "
        "    return acc; "
        "  end Read; "
        "  public static method Main(): int "
        "    var b := VBox.Some(42); "
        "    var f := lambda(): int => C.Read(b); "
        "    return f(); "
        "  end Main; "
        "end C;",
        42);
}
