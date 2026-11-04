// Codegen — #8011 (§8.6 / §28.3 / §28.9, v0.353): the §8.6 argument box the
// caller mints for an `Object` parameter is released after the call when the
// callee only passes the parameter ON, as a plain call argument, to a callee
// whose own parameter does not escape.
//
// §8.6 (v0.353): "When a value type is used where `Object` (or an interface
//   implemented by the value type) is expected, the value is boxed: a
//   heap-allocated wrapper holds the value. Boxed values are runtime-managed
//   ([§28.9])."
// §28.3 (v0.353): the box minted for an argument is an unbound owned
//   temporary whose lifetime ends at the call return; a call argument is a
//   borrow for the duration of the call.
//
// THE DEFECT. #2952 releases the argument box at the post-call point only
// when ParamEscapeWalker proves the callee's parameter non-escaping. The
// walker's call arm marked EVERY parameter passed on as a call argument as
// escaping, so `Tag(5)` below leaked its 24-byte box on every call even though
// `Helper` only borrows its argument for the duration of the nested call.
//
// THE ISSUE'S OWN REPRO IS INERT. It declares `Tag<Object>(x: Object)`: since
// e4355ef9 the type parameter `Object` SHADOWS the root `Object`, so the
// parameter is a bare type parameter, no §8.6 box is minted, and nothing
// leaks (the 24 B the original reviewer measured was #8008's shadowing
// symptom). The rows below use the CORRECTED repro (a non-generic `Tag`),
// which leaks 24 B per call on the pre-fix compiler (MEASURED: LeakSanitizer,
// one `calloc(1, 24)`, zero `bz_box_release`).
//
// THE FIX defers the walker's verdict to the CALLEE: a plain positional
// argument naming the tracked parameter escapes only when the callee's
// parameter at that position escapes (`isCalleeParamNonEscaping`, recursively).
// It answers only for a callee with STATIC dispatch whose body this module
// compiles; a virtual / abstract / non-`sealed` override / interface member, an extension,
// an operator, an `async` method, an iterator, an imported callee, or a
// `ref`/`out`/`move`/named argument keeps the old escape verdict.
//
// OVER-RELEASE GUARDS. The guard rows pass the parameter to a callee that
// genuinely lets it escape (field store, return, virtual override that
// stores, recursion). They run under ASan with leak detection OFF, and READ
// the stored box after the outer call returns, so a premature release
// surfaces as a heap-use-after-free / wrong value, not as a missing leak.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The corrected repro (the verifier's P9), looped 100x so the leak oracle is
// not one-shot: 100 x 24 B leaked before the fix.
const char* kCorrectedRepro =
    "module M8011A; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + Tag(5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag(o: Object): int "
    "    var h := Helper(o); "
    "    return h + ((o as int) ?? 0); "
    "  end Tag; "
    "  public static method Helper(o: Object): int => 7; "
    "end Program;";

// The issue-shaped body: the nested call sits inside `Console.WriteLine`.
const char* kInsideWriteLine =
    "module M8011B; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + Tag(5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag(o: Object): int "
    "    Console.WriteLine(Helper(o)); "
    "    return (o as int) ?? 0; "
    "  end Tag; "
    "  public static method Helper(o: Object): int => 7; "
    "end Program;";

// The qualified spelling `Program.Helper(o)` and a two-level chain
// Tag -> Mid -> Leaf, where Leaf reads the payload.
const char* kQualifiedChain =
    "module M8011C; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + Tag(5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag(o: Object): int => Program.Mid(o); "
    "  public static method Mid(o: Object): int "
    "    return Leaf(o) + 1; "
    "  end Mid; "
    "  public static method Leaf(o: Object): int "
    "    return (o as int) ?? 0; "
    "  end Leaf; "
    "end Program;";

// GUARD 1: the nested callee STORES its parameter into a field; the stored
// box is read after the outer call returns.
const char* kGuardFieldStore =
    "module M8011D; "
    "public class Sink "
    "  public _saved: Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Store(v: Object): int "
    "    _saved := v; "
    "    return 1; "
    "  end Store; "
    "end Sink; "
    "public class Program "
    "  public static method Tag(s: Sink, o: Object): int "
    "    return s.Store(o); "
    "  end Tag; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var a := Tag(s, 7); "
    "    var b := (s._saved as int) ?? 0; "
    "    var h := s._saved.ToString(); "
    "    Console.WriteLine($\"{a} {b} {h}\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 2: the nested callee is VIRTUAL. The base body only reads, but the
// override that actually runs stores the parameter; the static target's body
// must not decide the release.
const char* kGuardVirtualOverride =
    "module M8011E; "
    "public class Base "
    "  public constructor() end constructor; "
    "  public virtual method Put(v: Object): int => 0; "
    "end Base; "
    "public class Keeper: Base "
    "  public _saved: Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public override method Put(v: Object): int "
    "    _saved := v; "
    "    return 1; "
    "  end Put; "
    "end Keeper; "
    "public class Program "
    "  public static method Tag(b: Base, o: Object): int "
    "    return b.Put(o); "
    "  end Tag; "
    "  public static method Main(): int "
    "    using var k := new Keeper(); "
    "    var a := Tag(k, 9); "
    "    var b := (k._saved as int) ?? 0; "
    "    var h := k._saved.ToString(); "
    "    Console.WriteLine($\"{a} {b} {h}\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 3: the nested callee RETURNS its parameter, and the outer callee
// stores the result in a field that outlives the call.
const char* kGuardReturnThenStore =
    "module M8011F; "
    "public class Sink "
    "  public _saved: Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Tag(o: Object): int "
    "    _saved := Pass(o); "
    "    return 1; "
    "  end Tag; "
    "  public static method Pass(v: Object): Object => v; "
    "end Sink; "
    "public class Program "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var a := s.Tag(11); "
    "    var b := (s._saved as int) ?? 0; "
    "    var h := s._saved.ToString(); "
    "    Console.WriteLine($\"{a} {b} {h}\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 4: mutual recursion between two callees that pass the parameter on;
// the analysis must terminate, and a cycle is answered conservatively.
const char* kGuardRecursion =
    "module M8011G; "
    "public class Program "
    "  public static method Even(n: int, o: Object): int "
    "    if n = 0 then return (o as int) ?? 0; end if; "
    "    return Odd(n - 1, o); "
    "  end Even; "
    "  public static method Odd(n: int, o: Object): int "
    "    if n = 0 then return 0; end if; "
    "    return Even(n - 1, o); "
    "  end Odd; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<10 do t := t + Even(4, 3); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 5 (the rejected review's F1, p11 looped): the nested callee captures
// its parameter in a LAMBDA stored in a field; the closure runs after the
// outer call returns. §27.5 (v0.355): "Captured variables are heap-allocated
// in a closure object", so the box outlives the call.
const char* kGuardLambdaCapture =
    "module M8011H; "
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
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      var f := s._f!; "
    "      if f() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 6 (the review's p11c): the captured parameter is read through an
// interpolation hole inside the lambda.
const char* kGuardLambdaInterp =
    "module M8011I; "
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
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      var f := s._f!; "
    "      if f() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 7 (the review's p11d): the captured parameter is only TYPE-TESTED
// inside the lambda. An early release is a silent wrong value here ("other"
// instead of "int"), not an ASan report, so the row pins the stdout.
const char* kGuardLambdaIsTest =
    "module M8011J; "
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
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      var f := s._f!; "
    "      if f() = \"int\" then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 8 (the review's p20, looped): mutual recursion whose ODD arm STORES
// the parameter. Pins the cycle's escape answer: a cycle back to a callee
// still being analysed is answered "escapes", so neither callee is cached
// non-escaping.
const char* kGuardMutualStore =
    "module M8011K; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Even(s: Sink, n: int, o: Object): int "
    "    if n = 0 then return 0; end if; "
    "    return Odd(s, n - 1, o); "
    "  end Even; "
    "  public static method Odd(s: Sink, n: int, o: Object): int "
    "    if n = 0 then s._saved := o; return 1; end if; "
    "    return Even(s, n - 1, o); "
    "  end Odd; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Even(s, 3, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 9: the nested callee stores its parameter through a §17.15 binding
// atom (the fix-forward of ca822b0b's bind arm, reached through a pass-on).
const char* kGuardBindStore =
    "module M8011L; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._saved := (var t := o; t); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 10: the nested callee stores its parameter through a call whose
// result is DISCARDED (§17.13), reached through a pass-on.
const char* kGuardDiscardStore =
    "module M8011M; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Put(o: Object): int "
    "    _saved := o; "
    "    return 1; "
    "  end Put; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    _ := s.Put(o); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// RELEASE through the fix-forward's arms: the nested callee only type-tests
// its parameter inside a discard and a binding atom, so the pass-on does not
// escape and the box is released (100 x 24 B leaked before the fix).
const char* kPassOnBindDiscardRead =
    "module M8011N; "
    "public class Program "
    "  public static method Look(o: Object): int "
    "    _ := (if o is int then 1 else 0); "
    "    var n := (var t := 1; (if o is int then t else 0)); "
    "    return n; "
    "  end Look; "
    "  public static method Tag(o: Object): int => Look(o); "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      n := n + Tag(i + 1000); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// RELEASE through a `sealed override`: the static target's body is the one
// that runs (no further override), and it only type-tests the parameter.
const char* kPassOnSealedRead =
    "module M8011O; "
    "public class Base "
    "  public constructor() end constructor; "
    "  public virtual method Look(v: Object): int => 0; "
    "end Base; "
    "public class Leaf: Base "
    "  public constructor() end constructor; "
    "  public sealed override method Look(v: Object): int "
    "    => (if v is int then 1 else 0); "
    "end Leaf; "
    "public class Program "
    "  public static method Tag(k: Leaf, o: Object): int => k.Look(o); "
    "  public static method Main(): int "
    "    using var k := new Leaf(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      n := n + Tag(k, i + 1000); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// per §8.6 / §28.3, v0.353 — the corrected repro: the argument box of
// `Tag(5)` is released after each call (100 x 24 B leaked before the fix).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           corrected_repro_pass_on_releases_box_spec_8_6) {
    JitResult plain = runAot(kCorrectedRepro);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("1200\n"));
    JitResult r = runAotChecked(kCorrectedRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1200\n"));
}

// per §8.6 / §28.3, v0.353 — the issue's body shape: the nested call is an
// argument of `Console.WriteLine`.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           pass_on_inside_writeline_releases_box_spec_8_6) {
    std::string expect;
    for (int i = 0; i < 100; ++i) expect += "7\n";
    expect += "500\n";
    JitResult plain = runAot(kInsideWriteLine);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, expect);
    JitResult r = runAotChecked(kInsideWriteLine);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expect);
}

// per §8.6 / §28.3, v0.353 — a qualified call and a two-level chain: the
// non-escape verdict is transitive.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           qualified_two_level_chain_releases_box_spec_8_6) {
    JitResult plain = runAot(kQualifiedChain);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("600\n"));
    JitResult r = runAotChecked(kQualifiedChain);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));
}

// per §28.3, v0.353 — GUARD: the nested callee stores the parameter; the box
// is kept alive (no use-after-free; leak detection off).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_field_store_keeps_box_spec_28_3) {
    JitResult r = runAotCheckedLenient(kGuardFieldStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 7 7\n"));
}

// per §28.3 / §12, v0.353 — GUARD: a virtual nested callee whose override
// stores the parameter; the base body does not decide the release.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_virtual_override_store_keeps_box_spec_28_3) {
    JitResult r = runAotCheckedLenient(kGuardVirtualOverride);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 9 9\n"));
}

// per §28.3, v0.353 — GUARD: the nested callee returns the parameter and the
// result is stored in a field; the box is kept alive.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_return_then_store_keeps_box_spec_28_3) {
    JitResult r = runAotCheckedLenient(kGuardReturnThenStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 11 11\n"));
}

// per §28.3, v0.353 — GUARD: mutual recursion terminates and computes the
// right value.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_mutual_recursion_terminates_spec_28_3) {
    JitResult r = runAotCheckedLenient(kGuardRecursion);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("30\n"));
}

// Rows below added with the rebase onto the fix-forward of ca822b0b
// (§8.6 / §27.5 / §12.1, v0.355). Every guard loops 100x inside the program
// and pins the exact stdout; under the ASan leg an early release is a
// heap-use-after-free (or, for the type-test row, a wrong count).

// per §27.5 / §8.6, v0.355 — GUARD (review F1): a lambda in the nested callee
// captures the parameter; the closure runs after the outer call returns.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_lambda_capture_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kGuardLambdaCapture);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §27.5 / §8.6, v0.355 — GUARD: the captured parameter is read through an
// interpolation hole inside the lambda.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_lambda_interpolation_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kGuardLambdaInterp);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §27.5 / §8.6, v0.355 — GUARD: the captured parameter is only type-tested
// inside the lambda; an early release reads a freed box's type tag.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_lambda_type_test_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kGuardLambdaIsTest);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §8.6, v0.355 — GUARD: mutual recursion whose arm STORES the parameter;
// the cycle is answered "escapes".
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_mutual_recursion_store_keeps_box_spec_8_6) {
    JitResult r = runAotCheckedLenient(kGuardMutualStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §17.15 / §8.6, v0.355 — GUARD: the nested callee stores the parameter
// through a binding atom.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_bind_atom_store_keeps_box_spec_17_15) {
    JitResult r = runAotCheckedLenient(kGuardBindStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §17.13 / §8.6, v0.355 — GUARD: the nested callee stores the parameter
// through a call whose result is discarded.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_discard_store_keeps_box_spec_17_13) {
    JitResult r = runAotCheckedLenient(kGuardDiscardStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §8.6, v0.355 — the pass-on reaches a callee that only type-tests the
// parameter inside a discard and a binding atom: the box is released.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           pass_on_bind_discard_read_releases_box_spec_8_6) {
    JitResult plain = runAot(kPassOnBindDiscardRead);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("100\n"));
    JitResult r = runAotChecked(kPassOnBindDiscardRead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §12.1 / §8.6, v0.355 — the pass-on reaches a `sealed override`, whose
// body is the one that runs and only type-tests the parameter: released.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           pass_on_sealed_override_read_releases_box_spec_12_1) {
    JitResult plain = runAot(kPassOnSealedRead);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("100\n"));
    JitResult r = runAotChecked(kPassOnSealedRead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}
