// Codegen — fix #9032 (§14.1, §27.5, v0.359): a generic method whose body
// declares a lambda that CALLS another lambda local (`var f := lambda() => p;
// var g := lambda() => f();`) failed IR verification when the method was
// instantiated from a method other than `Main`:
//
//   Function return type does not match operand type of return inst!
//     ret ptr %8
//    i32
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// THE DEFECT. A generic method is emitted TWICE — the erased template body
// (`T` lowered to its `int` placeholder, genericSubst_ empty) and one `$spec`
// body per instantiation (`T` -> `string`, `ptr`). `emitClosureCall` types the
// call `f()` from the callee local's `localGenericSubst_` entry (keyed by the
// local's AST decl node, shared by every emission of the body) and, failing
// that, from genericSubst_. Two pieces of state leaked from the `$spec`
// emission into the template emission that follows it:
//   1. `lastCallGenericSubst_` — the transient "a call publishes, the next
//      `var` decl consumes" channel. The caller's `t := t + Use<string>(s)` is
//      not a `var` decl, so nothing consumed {T -> ptr}; it survived into the
//      next body emitted, and lowerLocalDecl recorded it as `f`'s
//      localGenericSubst_ there;
//   2. `localGenericSubst_[f.decl]` itself, left by the `$spec` emission and
//      never overwritten by the template emission (empty genericSubst_).
// So inside the template's `i32`-returning lambda `g`, `f()` was lowered
// `call ptr`: `ret ptr` in an `i32` function. Which emission ran first decided
// it: instantiated from `Main` the template body was emitted before the
// instantiation existed and compiled (that arm is the control below).
//
// THE FIX (Setup.cpp): emitFunctionBody starts a body with nothing published
// in lastCallGenericSubst_, and lowerLocalDecl drops whatever an earlier
// emission recorded for the decl before recording its own.
//
// THE SPEC. §14.1: a generic body is emitted per instantiation and the result
// of one instantiation must not decide another's typing; §27.5: a lambda may
// capture a local that is itself a closure and call it.
//
// ORACLE. runAot (compile + link + run natively), asserting the printed value.
// One row runs under runAotChecked (ASan) over a 200-iteration loop.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectOutput(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The issue's repro, verbatim in shape: instantiated twice from `Run`, which
// `Main` calls. RED on r30-c1: IR verification failed.
BEZEL_TEST(generic_lambda_calls_lambda_local_9032,
           issue_repro_instantiated_from_a_non_main_method) {
    const std::string src =
        "module P; public class Program "
        "  public static method Use<T>(p: T): int "
        "    var f := lambda() => p; var g := lambda() => f(); "
        "    var r := g(); return 1; "
        "  end Use; "
        "  public static method Run(): int "
        "    var s := \"hi\" + \"x\"; var t := 0; "
        "    t := t + Program.Use<string>(s); "
        "    t := t + Program.Use<string>(s); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); return 0; "
        "  end Main; "
        "end Program;";
    expectOutput(runAot(src), "0\n");
}

// The same body in a loop; the result counts the iterations. RED on r30-c1.
BEZEL_TEST(generic_lambda_calls_lambda_local_9032,
           nested_lambda_call_in_a_loop_prints_the_count) {
    const std::string src =
        "module P; public class Program "
        "  public static method Use<T>(p: T): int "
        "    var f := lambda() => p; var g := lambda() => f(); "
        "    var r := g(); return 1; "
        "  end Use; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var s := \"hi\" + \"x\"; t := t + Program.Use<string>(s); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); return 0; "
        "  end Main; "
        "end Program;";
    expectOutput(runAot(src), "200\n");
}

// A `() -> T` generic method returning a lambda that calls a captured lambda
// (probe blk2): the returned closure is invoked by the caller. RED on r30-c1.
BEZEL_TEST(generic_lambda_calls_lambda_local_9032,
           returned_lambda_calls_a_captured_lambda_local) {
    const std::string src =
        "module P; public class Program "
        "  public static method Two<T>(p: T, q: T): () -> T "
        "    var a := lambda() => p; "
        "    var b := lambda() => q; "
        "    return lambda() => a(); "
        "  end Two; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var s := \"hi\" + \"x\"; "
        "      var h := Program.Two<string>(s, \"k\" + \"z\"); "
        "      t := t + h().Length; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); return 0; "
        "  end Main; "
        "end Program;";
    expectOutput(runAot(src), "600\n");
}

// A lambda capturing the RESULT of a call of another lambda local (probe
// o4_cap): `r` is bound from `f()`, `g` captures `r`.
BEZEL_TEST(generic_lambda_calls_lambda_local_9032,
           lambda_captures_the_result_of_a_lambda_local_call) {
    const std::string src =
        "module P; public class Program "
        "  public static method Sink<U>(u: U): int return 1; end Sink; "
        "  public static method Use<T>(p: T): int "
        "    var f := lambda() => p; var r := f(); "
        "    var g := lambda() => r; var q := r; "
        "    return Program.Sink<T>(g()) + Program.Sink<T>(q) "
        "         + Program.Sink<T>(r); "
        "  end Use; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var s := \"hi\" + \"x\"; t := t + Program.Use<string>(s); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); return 0; "
        "  end Main; "
        "end Program;";
    expectOutput(runAot(src), "600\n");
}

// The ASan + LeakSanitizer leg over a 200-iteration loop (probe o12_idpass): a
// `T`-returning generic method forwards a lambda's result through a generic
// identity, instantiated at `string` from `Run`. Clean under ASan/LSan with the
// fix (0 B leaked). The issue's own body (`g := lambda() => f()`) is NOT used
// for the leak oracle: its non-generic twin leaks the captured closure cell
// identically (8 B/call, the open capture-cell family, #4542 / #8984), which is
// a different defect from the verifier failure pinned here.
BEZEL_TEST(generic_lambda_calls_lambda_local_9032,
           lambda_result_through_a_generic_identity_is_clean_under_asan) {
    const std::string src =
        "module P; public class Program "
        "  public static method Id<T>(p: T): T return p; end Id; "
        "  public static method Use<T>(p: T): T "
        "    var f := lambda() => p; "
        "    var r := Program.Id<T>(f()); var q := r; return q; "
        "  end Use; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var s := \"hi\" + \"x\"; t := t + Program.Use<string>(s).Length; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); return 0; "
        "  end Main; "
        "end Program;";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectOutput(r, "600\n");
}

// CONTROL (green on r30-c1): instantiated from `Main` itself, the template
// body is emitted before the instantiation exists, so nothing leaked into it.
BEZEL_TEST(generic_lambda_calls_lambda_local_9032,
           control_instantiated_from_main_compiles) {
    const std::string src =
        "module P; public class Program "
        "  public static method Use<T>(p: T): int "
        "    var f := lambda() => p; var g := lambda() => f(); "
        "    var r := g(); return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    var s := \"hi\" + \"x\"; var t := 0; "
        "    t := t + Program.Use<string>(s); "
        "    Console.WriteLine(t); return 0; "
        "  end Main; "
        "end Program;";
    expectOutput(runAot(src), "1\n");
}

}  // namespace
