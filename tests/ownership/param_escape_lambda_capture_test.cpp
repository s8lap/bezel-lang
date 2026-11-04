// Ownership / codegen — §8.6 / §27.5 / §28.3 / §28.9 (v0.355), #8095 (narrow
// #4841): a lambda that CAPTURES an `Object` / interface parameter keeps the
// parameter's §8.6 argument box alive past the call, so the callee's escape
// walk (`ParamEscapeWalker`, src/codegen/LlvmBackend_Assign.cpp) must report
// the parameter as ESCAPING, and the caller must NOT release the box after the
// call.
//
// THE DEFECT (MEASURED on staging f9e23eba, which contains ca822b0b). The
// walker treated the parameter as a SAFE READ in its receiver-call arm
// (`v.ToString()`), its type-test arm (`v is int`) and its interpolation arm
// (`$"{v}"`) without checking whether it was inside a lambda body. Inside a
// lambda each of those is a §27.5 capture ("Captured variables are heap-
// allocated in a closure object"): the closure holds the box, may be stored in
// a field, and is invoked after the call returned. The caller's post-call
// `bz_box_release` (#2952 for a method; ca822b0b for a constructor) then
// freed a box the closure still read: a heap-use-after-free, or (a12) a wrong
// value read from the freed box.
//
// THE FIX. The walker asks the codegen's own capture set for each lambda
// (`Emitter::collectCapturedSyms`, the walk that decides what the closure env
// holds); a lambda that captures the tracked parameter is an escape whatever
// spelling its body uses. Every safe-read position is also refused while the
// walk is inside a lambda body. Two sibling gaps of the same walk are closed
// with it: a METHOD REFERENCE over the parameter (`v.ToString`, §27.6 captures
// the receiver) and an ITERATOR / ASYNC callee, whose frame holds every
// parameter past the call (§29.3 / §30.1).
//
// ORACLES. Rows 1-7 and 11-13 are the escaping shapes, each loop-amplified 100x and run
// on `runAotCheckedLenient`: generated-code ASan is active (a UAF fails the
// row), LeakSanitizer is OFF because the CONSERVATIVE direction is a leak (the
// field-held closure's box is #6827's family, not this row's subject). Each
// row asserts the exact stdout `100`, so a wrong value read from a freed box
// fails it without ASan too. Rows 8-10 are CONTROLS that must STAY
// non-escaping: run on `runAotChecked` (LeakSanitizer ON), 100x, so a fix that
// over-reported escapes (and stopped releasing the box) would leak and fail.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// (a6) a CONSTRUCTOR stores `f := lambda() => v.ToString()` in a field: the
// closure captures the parameter through a RECEIVER call. Base (pre-ca822b0b):
// leak; ca822b0b: heap-use-after-free.
const char* kCtorLambdaReceiver =
    "module M; "
    "public class C "
    "  public f: () -> string; "
    "  public constructor(v: Object) "
    "    f := lambda() => v.ToString(); "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var c := new C(i + 1000); "
    "      if c.f() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (a12) a CONSTRUCTOR stores `f := lambda() => v is int`: a TYPE-TEST operand
// inside a lambda. ca822b0b: prints 0 (the freed box's descriptor), not 100.
const char* kCtorLambdaTypeTest =
    "module M; "
    "public class C "
    "  public f: () -> bool; "
    "  public constructor(v: Object) "
    "    f := lambda() => v is int; "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var c := new C(i + 1000); "
    "      if c.f() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (a9) the METHOD twin of a6: `method Set(v: Object) f := lambda() =>
// v.ToString();`. A heap-use-after-free since #2952's method path, before
// ca822b0b too.
const char* kMethodLambdaReceiver =
    "module M; "
    "public class C "
    "  public f: () -> string := lambda() => \"\"; "
    "  public method Set(v: Object) "
    "    f := lambda() => v.ToString(); "
    "  end Set; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var c := new C(); "
    "      c.Set(i + 1000); "
    "      if c.f() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (p11b) a static `Keep(s, o: Object)` stores `lambda() => o.ToString()` into
// another object's field; called directly.
const char* kKeepDirect =
    "module P; "
    "public class Sink "
    "  public _f: (() -> string)?; "
    "  public constructor() _f := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._f := lambda() => o.ToString(); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var s := new Sink(); "
    "      var a := Keep(s, i + 1000); "
    "      var f := s._f!; "
    "      if a = 1 and f() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (p11) the same `Keep`, reached through a forwarding `Tag(s, o) => Keep(s,
// o)`. On staging f9e23eba this row is green with and without the fix: the
// walker judges `Tag`'s parameter escaping through its generic-call arm. It
// guards the forwarding relaxation (#8011), which must still see `Keep`'s
// capture as an escape.
const char* kKeepForwarded =
    "module P; "
    "public class Sink "
    "  public _f: (() -> string)?; "
    "  public constructor() _f := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._f := lambda() => o.ToString(); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var s := new Sink(); "
    "      var a := Tag(s, i + 1000); "
    "      var f := s._f!; "
    "      if a = 1 and f() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (p11c) the capture spelled as an INTERPOLATION hole: `lambda() => $"{o}"`.
const char* kKeepInterpolation =
    "module P; "
    "public class Sink "
    "  public _f: (() -> string)?; "
    "  public constructor() _f := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._f := lambda() => $\"{o}\"; "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var s := new Sink(); "
    "      var a := Keep(s, i + 1000); "
    "      var f := s._f!; "
    "      if a = 1 and f() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (p11d) the capture spelled as a type test in a conditional's condition:
// `lambda() => (if o is int then "int" else "other")`.
const char* kKeepConditionalTypeTest =
    "module P; "
    "public class Sink "
    "  public _f: (() -> string)?; "
    "  public constructor() _f := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._f := lambda() => (if o is int then \"int\" else \"other\"); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var s := new Sink(); "
    "      var a := Keep(s, i + 1000); "
    "      var f := s._f!; "
    "      if a = 1 and f() = \"int\" then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (control) a receiver read OUTSIDE any lambda: `Tag := v.ToString()`. Non-
// escaping; the box must still be released (leak-checked).
const char* kCtrlPlainReceiverRead =
    "module M; "
    "public class C "
    "  public readonly property Tag: string; "
    "  public constructor(v: Object) "
    "    Tag := v.ToString(); "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var c := new C(i + 1000); "
    "      if c.Tag = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (control) a lambda in the same body that does NOT capture the parameter.
// Non-escaping; the box must still be released (leak-checked).
const char* kCtrlLambdaWithoutCapture =
    "module M; "
    "public class C "
    "  public readonly property Tag: string; "
    "  public f: () -> string; "
    "  public constructor(v: Object) "
    "    Tag := v.ToString(); "
    "    f := lambda() => \"k\"; "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var c := new C(i + 1000); "
    "      if c.Tag = (i + 1000).ToString() and c.f() = \"k\" then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (control) a type test OUTSIDE any lambda: `IsInt(v: Object): bool => v is
// int`. Non-escaping; leak-checked.
const char* kCtrlTypeTestOutsideLambda =
    "module M; "
    "public class Program "
    "  public static method IsInt(v: Object): bool => v is int; "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      if IsInt(i) then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (x1) a CONSTRUCTOR stores the METHOD REFERENCE `f := v.ToString;`: §27.6
// "the instance is captured at formation". The walker had no arm for a non-
// call member access and judged it nothing at all; staging: heap-use-after-
// free.
const char* kCtorMethodReference =
    "module M; "
    "public class C "
    "  public f: () -> string; "
    "  public constructor(v: Object) "
    "    f := v.ToString; "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var c := new C(i + 1000); "
    "      if c.f() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (x3) the capture sits in a NESTED lambda: `f := lambda() => lambda() =>
// v.ToString();`.
const char* kCtorNestedLambda =
    "module M; "
    "public class C "
    "  public f: () -> () -> string; "
    "  public constructor(v: Object) "
    "    f := lambda() => lambda() => v.ToString(); "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var c := new C(i + 1000); "
    "      var g := c.f(); "
    "      if g() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (x2) an ITERATOR method `Gen(v: Object): IEnumerable<string>` whose body
// `yield return v.ToString();` runs after the call returned (§29.3); staging:
// heap-use-after-free.
const char* kIteratorParam =
    "module M; "
    "public class Program "
    "  public static method Gen(v: Object): IEnumerable<string> "
    "    yield return v.ToString(); "
    "  end Gen; "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      for var s in Gen(i + 1000) do "
    "        if s = (i + 1000).ToString() then n := n + 1; end if; "
    "      end for; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (p11d verbatim) the #8011 review's probe exactly as filed: one call through
// the forwarding `Tag`, the capture spelled as a type test in a conditional's
// condition. Must print "1 int". On staging f9e23eba it is green with and
// without this fix (the forwarding call escapes through the generic-call
// arm); it guards the #8011 forwarding relaxation once that is rebased here.
const char* kP11dVerbatim =
    "module P11d; "
    "public class Sink "
    "  public _f: (() -> string)?; "
    "  public _saved: Object?; "
    "  public constructor() _f := null; _saved := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._f := lambda() => (if o is int then \"int\" else \"other\"); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var a := Tag(s, 7); "
    "    var f := s._f!; "
    "    Console.WriteLine($\"{a} {f()}\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, ctor_lambda_receiver_capture_keeps_box_spec_27_5_8_6) {
    JitResult r = runAotCheckedLenient(kCtorLambdaReceiver);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, ctor_lambda_type_test_capture_keeps_box_spec_27_5_12_7) {
    JitResult r = runAotCheckedLenient(kCtorLambdaTypeTest);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, method_lambda_receiver_capture_keeps_box_spec_27_5_8_6) {
    JitResult r = runAotCheckedLenient(kMethodLambdaReceiver);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, static_keep_lambda_capture_direct_call_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kKeepDirect);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, static_keep_lambda_capture_forwarded_call_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kKeepForwarded);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, static_keep_lambda_interpolation_capture_keeps_box_spec_27_5_1_5) {
    JitResult r = runAotCheckedLenient(kKeepInterpolation);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, static_keep_lambda_conditional_type_test_capture_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kKeepConditionalTypeTest);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, control_receiver_read_outside_lambda_still_releases_box_spec_28_3) {
    JitResult r = runAotChecked(kCtrlPlainReceiverRead);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, control_non_capturing_lambda_still_releases_box_spec_28_3) {
    JitResult r = runAotChecked(kCtrlLambdaWithoutCapture);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, control_type_test_outside_lambda_still_releases_box_spec_28_3) {
    JitResult r = runAotChecked(kCtrlTypeTestOutsideLambda);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, ctor_method_reference_keeps_box_spec_27_6_8_6) {
    JitResult r = runAotCheckedLenient(kCtorMethodReference);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, ctor_nested_lambda_capture_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kCtorNestedLambda);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095, iterator_method_param_keeps_box_spec_29_3_8_6) {
    JitResult r = runAotCheckedLenient(kIteratorParam);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_lambda_capture_8095,
           p11d_verbatim_forwarded_conditional_type_test_prints_1_int) {
    JitResult r = runAotCheckedLenient(kP11dVerbatim);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1 int\n");
}
