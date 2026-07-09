// fix #1306 / §27.6 / §26.1 / §9.2: an EXTENSION-method reference bound to a
// RECORD receiver — `Vec.Scaled` used as a `(Vec) -> int` value — must lower to
// a valid function value whose thunk forwards the caller-supplied record as the
// method's `this`. §27.6: "For an extension method, the reference uses the
// extended type's name as qualifier; the resulting function has the receiver as
// its first parameter." A record is a value type (§9.2), so the receiver is the
// function's first PARAMETER (passed by value, never captured).
//
// BUG (#1306): the method-reference lowering treated an extension method like a
// bound INSTANCE-method ref — capturing a receiver from the qualifier. But the
// qualifier (`Vec`) is a TYPE NAME, not an instance, so the "captured receiver"
// was null and the thunk loaded `this` through a null env pointer → SIGSEGV at
// the call. (On main the symptom had morphed to a false §5.1 rejection: sema
// typed the reference `() -> int` instead of `(Vec) -> int`, dropping the
// receiver parameter.) Both the sema typing (prepend the receiver as the first
// function parameter) and the codegen (unbound thunk: env null, receiver
// forwarded and shaped to the method's `this` ABI — record spilled to a pointer)
// are verified here.
//
// These tests JIT-run programs that take an extension-method reference on a
// record and call it; without the fix they fail to compile (-999) or crash.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// Run a whole module source and return Main's exit code, or a sentinel.
int runMod(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §27.6 (#1306): `Vec.Scaled` taken as a `(Vec) -> int` value and called — the
// core crash. Without the fix: SIGSEGV (rc=139) / false §5.1 rejection (-999).
// Expected 42 (Scaled(v) = v.x * 3 = 14 * 3).
BEZEL_TEST(codegen_extension_method_ref_record_1306, scaled_record_ref_1306) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Vec(x: int) end; "
        "extension Vec "
        "  public method Scaled(): int => this.x * 3; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var v := new Vec(14); "
        "    var f: (Vec) -> int := Vec.Scaled; "
        "    return f(v); "
        "  end Main; "
        "end C;"), 42);
}

// §27.6: the reference's signature carries the receiver PLUS the method's
// explicit parameters — `Vec.Plus(n)` is `(Vec, int) -> int`. Verifies the
// thunk forwards BOTH the record receiver (shaped to a spilled pointer) and the
// explicit argument.
BEZEL_TEST(codegen_extension_method_ref_record_1306, record_ref_with_explicit_param) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Vec(x: int) end; "
        "extension Vec "
        "  public method Plus(n: int): int => this.x + n; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (Vec, int) -> int := Vec.Plus; "
        "    return f(new Vec(20), 22); "  // 20 + 22 = 42
        "  end Main; "
        "end C;"), 42);
}

// §27.6 / §27.7: the function value is callable multiple times with different
// receivers — each call shapes the caller's record to the method's `this`.
BEZEL_TEST(codegen_extension_method_ref_record_1306, record_ref_multiple_calls) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Vec(x: int) end; "
        "extension Vec "
        "  public method Scaled(): int => this.x * 3; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (Vec) -> int := Vec.Scaled; "
        "    var sum := f(new Vec(10)); "   // 30
        "    sum := sum + f(new Vec(4)); "  // 12
        "    return sum; "                  // 42
        "  end Main; "
        "end C;"), 42);
}

// Regression: a DIRECT extension call on a record (`v.Scaled()`) must still
// work — the #1306 note isolates the bug to the method-REFERENCE lowering, so
// the direct-call path (a different codegen route) is verified unaffected.
BEZEL_TEST(codegen_extension_method_ref_record_1306, direct_record_extension_call_regression) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Vec(x: int) end; "
        "extension Vec "
        "  public method Scaled(): int => this.x * 3; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var v := new Vec(14); "
        "    return v.Scaled(); "  // 42
        "  end Main; "
        "end C;"), 42);
}

// Regression (§27.6 / #881): a bound CLASS instance-method reference must still
// capture its receiver (the case that already worked). Guards against the
// extRef change perturbing the bound-instance path.
BEZEL_TEST(codegen_extension_method_ref_record_1306, class_bound_method_ref_regression) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "public class Box(n: int) "
        "  public method GetN(): int => n; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(42); "
        "    var f: () -> int := b.GetN; "
        "    return f(); "
        "  end Main; "
        "end C;"), 42);
}

// Regression (§27.6): a STATIC-method reference must remain a capture-free
// function value (env null). Guards the static path against the extRef change.
BEZEL_TEST(codegen_extension_method_ref_record_1306, static_method_ref_regression) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class MathH "
        "  public static method Triple(n: int): int => n * 3; "
        "  public static method Main(): int "
        "    var f: (int) -> int := MathH.Triple; "
        "    return f(14); "  // 42
        "  end Main; "
        "end MathH;"), 42);
}
