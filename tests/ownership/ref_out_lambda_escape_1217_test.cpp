// Spec-derived tests for issue #1217 — reject a lambda that captures a
// `ref` or `out` parameter and ESCAPES the method.
//
// Spec basis: §27.5 / §22.3 / §28.6
//
//   A `ref`/`out` parameter (§22.3) is a pointer into the CALLER's stack
//   frame — valid only for the duration of the call. A lambda may capture
//   it by pointer (§27.5: "modifications inside the lambda are visible
//   outside"); that is sound as long as the lambda does NOT outlive the
//   call. Escape routes that let the lambda outlive the call:
//
//     (a) RETURNED from the method  — the caller holds the closure after
//         the stack frame (and the ref/out pointer it captured) is gone.
//     (b) STORED IN A FIELD        — the field outlives the call.
//     (c) WRITTEN THROUGH OUT-PARAM — the out-param's storage is the
//         caller's frame, which also outlives this call.
//
//   Non-escaping captures (lambda invoked locally, passed to a borrow
//   parameter) are sound — the closure is live only during the call —
//   and must remain accepted.
//
// Test structure:
//   NEGATIVE: escaping cases — must produce a §27.5/§28.6/#1217 error.
//   POSITIVE (controls): non-escaping cases — must compile without error.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Wraps a method body inside a class. No disposable resource is needed
// because a ref/out param is a pointer hazard, not an ownership hazard.
std::string wrapMethod(const std::string& method) {
    return "module M; class C " + method + " end C;";
}

// Wraps a method body inside a class that also declares a non-owned
// function-typed field `_handler` — used for field-store escape tests.
std::string wrapWithField(const std::string& method) {
    return "module M; class C "
           "  private _handler: (() -> int)? := null; "
           + method + " end C;";
}

// True iff the diagnostics include a §27.5/§28.6 ref/out capture escape
// error mentioning the given parameter name.
bool rejectsRefOutEscape(const bezel::test::OwnershipResult& r,
                         const std::string& paramName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("#1217") == std::string::npos) continue;
        if (d.message.find(paramName) == std::string::npos) continue;
        return true;
    }
    return false;
}

bool rejectsExternOutCallbackCapture(
    const bezel::test::OwnershipResult& r, const std::string& bindingName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§32.5") == std::string::npos) continue;
        if (d.message.find("extern call") == std::string::npos) continue;
        if (d.message.find("same call") == std::string::npos) continue;
        if (d.message.find(bindingName) == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff the diagnostics include the §32.8 (v0.307) plain-callback
// admission rejection: a capturing lambda / bound method at a plain
// (un-`env`-paired) extern function-typed parameter (#4671).
bool rejectsPlainExternCallbackCapture(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§32.8") == std::string::npos) continue;
        if (d.message.find("plain (un-`env`-paired) extern function-typed "
                           "parameter must be capture-free") ==
            std::string::npos)
            continue;
        if (d.message.find("capturing lambda") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — escaping ref/out-capturing lambdas must be rejected (#1217)
// =============================================================================

// §27.5 / §28.6 (#1217): the primary repro — `return () => x` where `x` is
// a `ref` parameter. The closure captures the ref-param pointer; returning
// the closure lets it outlive the call frame — a use-after-free. Per the
// issue this currently compiles silently and produces a latent UAF.
BEZEL_TEST(ref_out_lambda_escape_1217,
           return_lambda_capturing_ref_param_rejected_27_5) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public static method MakeAdder(ref x: int): () -> int "
        "  return lambda() => x; "
        "end MakeAdder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsRefOutEscape(r, "x"));
}

// §27.5 / §28.6 (#1217): same as above for an `out` parameter. An out-param
// is also a pointer into the caller's storage; returning a closure that
// captures it is the same use-after-free hazard.
BEZEL_TEST(ref_out_lambda_escape_1217,
           return_lambda_capturing_out_param_rejected_27_5) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public static method MakeWriter(out x: int): () -> int "
        "  x := 0; "
        "  return lambda() => x; "
        "end MakeWriter;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsRefOutEscape(r, "x"));
}

// §27.5 / §28.6 (#1217): store a lambda capturing a ref param into a field.
// The field outlives the call — same dangling-pointer hazard.
BEZEL_TEST(ref_out_lambda_escape_1217,
           store_lambda_in_field_capturing_ref_param_rejected_27_5) {
    auto r = parseAndCheckOwnership(wrapWithField(
        "public method SetHandler(ref x: int) "
        "  _handler := lambda() => x; "
        "end SetHandler;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsRefOutEscape(r, "x"));
}

// §27.5 / §28.6 (#1217): store a lambda capturing an out param into a field.
BEZEL_TEST(ref_out_lambda_escape_1217,
           store_lambda_in_field_capturing_out_param_rejected_27_5) {
    auto r = parseAndCheckOwnership(wrapWithField(
        "public method SetProducer(out x: int) "
        "  x := 0; "
        "  _handler := lambda() => x; "
        "end SetProducer;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsRefOutEscape(r, "x"));
}

// =============================================================================
// POSITIVE — non-escaping ref/out-capturing lambdas must still compile (#1207)
// =============================================================================

// §27.5 / §22.3 CONTROL (#1207): a lambda capturing a ref param that is
// invoked LOCALLY — the closure's lifetime is bounded by the method body;
// it does not escape. This is exactly the #1207 pattern that was fixed. The
// returned expression is the RESULT of calling the lambda (an `int` copy),
// NOT the lambda itself, so there is no pointer escape.
BEZEL_TEST(ref_out_lambda_escape_1217,
           local_invocation_ref_capturing_lambda_clean_27_5) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public static method Bump(ref x: int): int "
        "  var f := lambda() => do x := x + 1; return x; end; "
        "  return f(); "    // returns the int result, not the closure
        "end Bump;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.5 / §22.3 CONTROL: same as above for an out param — local invocation,
// not an escape.
BEZEL_TEST(ref_out_lambda_escape_1217,
           local_invocation_out_capturing_lambda_clean_27_5) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public static method Write(out x: int): int "
        "  x := 0; "
        "  var f := lambda() => do x := x + 7; return x; end; "
        "  return f(); "    // returns the int result, not the closure
        "end Write;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.5 CONTROL: a lambda that captures a NON-ref/out local and is returned
// escapes, but this is handled by the existing §28.9 / §28.3 rules (only when
// the local is owned). For a plain value-typed local (int), returning the
// lambda is legal — the captured value is copied into the closure cell and has
// no lifetime obligation. This tests that the #1217 check does NOT affect the
// non-ref/out capture path.
BEZEL_TEST(ref_out_lambda_escape_1217,
           return_lambda_capturing_value_local_unaffected_27_5) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public static method MakeAdder(n: int): () -> int "
        "  return lambda() => n; "    // n is a plain value param — legal escape
        "end MakeAdder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.5 CONTROL: a lambda that captures NOTHING and is returned is legal.
BEZEL_TEST(ref_out_lambda_escape_1217,
           return_non_capturing_lambda_clean_27_5) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public static method MakeConst(): () -> int "
        "  return lambda() => 42; "
        "end MakeConst;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// #1719 — §32.5 same-call extern out-recipient/callback capture intersection.
// Native code may invoke the callback before assigning the out recipient, so a
// callback that captures that binding would read indeterminate bits.
// =============================================================================

BEZEL_TEST(extern_out_callback_capture_issue_1719,
           direct_lambda_capturing_same_out_binding_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "extern \"c\" "
        "  public method fill(out value: int, cb: () -> int): int; "
        "end; "
        "class C public static method Main(): int "
        "  var x: int; "
        "  _ := fill(out x, lambda() => x); "
        "  return x; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsExternOutCallbackCapture(r, "x"));
}

// §32.5 (#1719): a never-reassigned local initialized from a lambda has a
// statically visible capture set too; spelling the callback through that local
// must not launder the same-call out-recipient read.
BEZEL_TEST(extern_out_callback_capture_issue_1719,
           traced_lambda_local_capturing_same_out_binding_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "extern \"c\" "
        "  public method fill(out value: int, cb: () -> int): int; "
        "end; "
        "class C public static method Main(): int "
        "  var x: int; "
        "  var cb := lambda() => x; "
        "  _ := fill(out x, cb); "
        "  return x; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsExternOutCallbackCapture(r, "x"));
}

// §32.5 / §22.4 (#1719): an inline `out var` introduces the same recipient
// binding for the whole call; a sibling callback still may not capture it.
BEZEL_TEST(extern_out_callback_capture_issue_1719,
           inline_out_var_captured_by_callback_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "extern \"c\" "
        "  public method fill(out value: int, cb: () -> int): int; "
        "end; "
        "class C public static method Main(): int "
        "  _ := fill(out var x, lambda() => x); "
        "  return x; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsExternOutCallbackCapture(r, "x"));
}

// §32.5 (#1719): the intersection is call-wide, not left-to-right. The
// callback may precede the out argument syntactically and must still be rejected.
BEZEL_TEST(extern_out_callback_capture_issue_1719,
           callback_before_same_out_argument_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "extern \"c\" "
        "  public method fill(cb: () -> int, out value: int): int; "
        "end; "
        "class C public static method Main(): int "
        "  var x: int; "
        "  _ := fill(lambda() => x, out x); "
        "  return x; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsExternOutCallbackCapture(r, "x"));
}

// §32.8 (v0.307): "At a **plain** (un-`env`-paired) function-typed parameter,
// the callback must be a **static method reference or a non-capturing
// lambda** ... A **bound instance-method reference** (`obj.Method`) and a
// **capturing lambda** are rejected there". `cb: () -> int` is plain, so this
// capturing lambda is rejected (#4671). §32.5's contrast — capturing the very
// binding passed `ref` to the same call is admitted — applies only at an
// `env`-paired parameter, the one place §32.8 admits a capturing lambda. This
// row was an accept control for #1719 and is restored as an env-paired accept
// row under #5050 once #4561 makes the `env` clause parse.
BEZEL_TEST(extern_out_callback_capture_issue_1719,
           direct_lambda_capturing_same_ref_binding_rejected_at_plain_param_spec_32_8) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "extern \"c\" "
        "  public method inspect(ref value: int, cb: () -> int): int; "
        "end; "
        "class C public static method Main(): int "
        "  var x := 7; "
        "  _ := inspect(ref x, lambda() => x); "
        "  return x; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsPlainExternCallbackCapture(r));
}

// §32.8 (v0.307): "At a **plain** (un-`env`-paired) function-typed parameter,
// the callback must be a **static method reference or a non-capturing
// lambda** ... A **bound instance-method reference** (`obj.Method`) and a
// **capturing lambda** are rejected there". `cb: () -> int` is plain, so a
// lambda capturing `y` is rejected (#4671) even though `y` is not the out
// recipient. §32.5's intersection rule — a callback may capture some other
// binding while the extern fills `x` through out — applies only at an
// `env`-paired parameter. This row was an accept control for #1719 and is
// restored as an env-paired accept row under #5050 once #4561 makes the `env`
// clause parse.
BEZEL_TEST(extern_out_callback_capture_issue_1719,
           callback_capturing_different_binding_rejected_at_plain_param_spec_32_8) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "extern \"c\" "
        "  public method fill(out value: int, cb: () -> int): int; "
        "end; "
        "class C public static method Main(): int "
        "  var x: int; "
        "  var y := 7; "
        "  _ := fill(out x, lambda() => y); "
        "  return x; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsPlainExternCallbackCapture(r));
}

// §32.5 (#1719): capture identity is symbol-based, not name-based. A lambda
// parameter that shadows the out recipient is local to the callback and is not
// a capture of the recipient binding.
BEZEL_TEST(extern_out_callback_capture_issue_1719,
           lambda_parameter_shadowing_out_name_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "extern \"c\" "
        "  public method fill(out value: int, cb: (int) -> int): int; "
        "end; "
        "class C public static method Main(): int "
        "  var x: int; "
        "  _ := fill(out x, lambda(x: int) => x); "
        "  return x; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
