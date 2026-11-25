// Regression tests for issue #9174 — a LAMBDA ARGUMENT passed to a method
// whose RETURN TYPE IS A FUNCTION VALUE was never released: one closure
// leaked per call.
//
// THE DEFECT.  §28.14 (v0.359) makes a plain (non-`scoped`) function-typed
// parameter CALL-BOUNDED: "the callee receives the callback for the duration
// of the call only … It may not be stored in a field of any object …, stored
// into any collection …, **returned**, captured by a closure that itself
// escapes the callee …, or forwarded into a `scoped` parameter position:
// … These obligations are compiler-verified for user-authored callees."  So
// the callee holds NO reference to the callback once the call returns, and the
// caller's own +1 on the closure cell (and on the cells its env points at) is
// the caller's to drop at the call's end.
//
// The release existed — the `.` call path's `prepareCallClosureArgRelease`
// call-bounded arm, and its capture-cell twin `isCallBoundedClosureArgPosition`
// — but BOTH were gated on the CALL'S RESULT not being a function value.  The
// gate's own reason was that "the callee may `return f;` directly", i.e. that
// the callee could hand the CALLER'S OWN closure back and a caller-side
// release would then free a live cell (the #1921 / #2248 UAF).  But that is
// exactly the sink §28.14's verification REJECTS: in a callee whose formal is
// a plain function-typed parameter, `return q;` is a compile error —
// MEASURED on this tree: "parameter `q` is call-bounded; declare it `scoped`
// to store it (§28.14, v0.177) — a plain function-typed parameter may be
// invoked, bound to locals, and passed to another plain function-typed
// parameter, but may not be returned".
//
// ⇒ a callee whose DECLARED return is a function type CANNOT return the
// argument: its function-typed RESULT is a DIFFERENT function value.  The
// gate was over-broad, and the caller's temporary closure leaked.
//
// MEASURED on the base (r30-c1 @ d7a045e3d) with the issue's own repro under
// `bezelc --sanitize=address` (LeakSanitizer):
//
//   Direct leak of 12800 byte(s) in 200 object(s)      // 64 B x 200
//   SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s).
//
// 64 bytes is the closure cell, once per call.  The CAPTURING twin leaks
// three allocations per call (the cell, its env block and the captured
// cell): 15200 bytes in 600 allocations.
//
// THE SPEC.  §28.14 (v0.359) *The call-bounded default* (quoted above) and
// §28.9 (v0.359) *Runtime-managed types*: a function value is runtime-managed
// — "the type's lifetime is managed by the runtime without user involvement" —
// and the RUNTIME-MANAGED value's reference is the producer's to discharge
// exactly once.
//
// ORACLE.  `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit (lane contract §5:
// disposal / ownership work runs under ASan).  Every row asserts the program's
// own output AND that LeakSanitizer counted nothing, so a silent zero-fill
// cannot pass.  The rows LOOP (200 iterations inside the program), so the leak
// cannot hide under the sanitizer's noise.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// The class under test: a method whose DECLARED RETURN TYPE IS A FUNCTION
// VALUE and whose plain function-typed parameter is call-bounded.  The callee
// returns a FRESH closure — never its argument (that is a compile error,
// §28.14) — so the result is a different function value from the argument.
const char* kFl =
    "public class Fl "
    "  public constructor() end constructor; "
    "  public method Get(q: () -> int): () -> int "
    "    return lambda() => 7; "
    "  end Get; "
    "end Fl; ";

// THE REGRESSION ROW: the issue's own repro, verbatim in shape, amplified
// 200x (the issue's own measurement).  The lambda ARGUMENT is the temporary
// that must be released when the call returns.
//
// RED on the base (12800 B / 200 allocations — the 64 B closure cell, once
// per call), GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           issue_repro_200_iterations_reclaims_the_call_bounded_argument) {
    const std::string src =
        "module M; import System; " + std::string(kFl) +
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      var f := fl.Get(lambda() => 5); "
        "      t := t + f(); "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// The DISCARDED-result sibling: the same call, its result thrown away.  The
// argument is still the caller's temporary, and the issue's own note that the
// discarded form "leaks nothing" is FALSE on the base — MEASURED: 12800 B /
// 200 allocations, the same 64 B closure cell per call.
//
// RED on the base, GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           discarded_result_200_iterations_reclaims_the_call_bounded_argument) {
    const std::string src =
        "module M; import System; " + std::string(kFl) +
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      _ := fl.Get(lambda() => 5); "
        "      t := t + 1; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// The RETURNED-result sibling: the call's result is returned from the
// enclosing method, so it outlives the call — but the ARGUMENT still does
// not (§28.14: the callee's hold ends with the call).
//
// RED on the base (12800 B / 200 allocations), GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           returned_result_200_iterations_reclaims_the_call_bounded_argument) {
    const std::string src =
        "module M; import System; " + std::string(kFl) +
        "public class Program "
        "  public static method Make(): () -> int "
        "    var fl := new Fl(); "
        "    return fl.Get(lambda() => 5); "
        "  end Make; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var f := Program.Make(); "
        "      t := t + f(); "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// The INVOKED-IMMEDIATELY sibling: the call's result is invoked in the same
// full expression, so it never reaches a local at all.
//
// RED on the base (12800 B / 200 allocations), GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           result_invoked_immediately_200_iterations_reclaims_the_argument) {
    const std::string src =
        "module M; import System; " + std::string(kFl) +
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      t := t + fl.Get(lambda() => 5)(); "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// The STATIC-method sibling: the receiver slot is empty, so the argument
// index shifts by one — the arm's own positional arithmetic must still find
// the argument.
//
// RED on the base (12800 B / 200 allocations), GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           static_method_200_iterations_reclaims_the_call_bounded_argument) {
    const std::string src =
        "module M; import System; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public static method Get(q: () -> int): () -> int "
        "    return lambda() => 7; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var f := Fl.Get(lambda() => 5); "
        "      t := t + f(); "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// The INTERFACE-method sibling: the call is dispatched virtually, so the
// callee the release is armed for is the INTERFACE's declaration, not the
// implementation's.  The implementation's own body is the one §28.14's
// verification ran on, and it cannot return its argument either.
//
// RED on the base (12800 B / 200 allocations), GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           interface_method_200_iterations_reclaims_the_call_bounded_argument) {
    const std::string src =
        "module M; import System; "
        "public interface IFl "
        "  method Get(q: () -> int): () -> int; "
        "end IFl; "
        "public class Fl : IFl "
        "  public constructor() end constructor; "
        "  public method Get(q: () -> int): () -> int "
        "    return lambda() => 7; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl: IFl := new Fl(); "
        "      var f := fl.Get(lambda() => 5); "
        "      t := t + f(); "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// The METHOD-GROUP sibling: a method reference is the other +1 closure
// producer (`isPlusOneClosureProducer` admits both), and it is captured by
// reference — the release must drop the caller's +1 on it too.
//
// RED on the base (12800 B / 200 allocations), GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           method_group_argument_200_iterations_reclaims_the_argument) {
    const std::string src =
        "module M; import System; " + std::string(kFl) +
        "public class Program "
        "  public static method Five(): int "
        "    return 5; "
        "  end Five; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      var f := fl.Get(Program.Five); "
        "      t := t + f(); "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// The CAPTURING sibling: the argument closure captures a local, so the
// closure's env points at a CAPTURED CELL — a second allocation, freed by
// the call site's capture-cell half (`consumeLastLambdaCapturedCells`), not
// by the closure-cell release.  Both halves were gated on the same
// over-broad result gate, so both leaked.
//
// RED on the base (15200 B / 600 allocations — the 64 B cell, the 8 B env
// and the 4 B captured cell, per call), GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           capturing_argument_200_iterations_reclaims_the_closure_and_its_cell) {
    const std::string src =
        "module M; import System; " + std::string(kFl) +
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      var x := 5; "
        "      var f := fl.Get(lambda() => x); "
        "      t := t + f(); "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// The ALIAS-SPELLED twin: the same shape with the function type named
// (`type F := () -> int;`), the spelling §28.14's verification and codegen
// both read alias-transparently (a Named TypeRef that resolves to a function
// type).  The result-side gate read the SPELLING, so the alias spelling
// leaked exactly like the inline one.
//
// RED on the base (12800 B / 200 allocations), GREEN with the fix.
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           alias_spelled_function_type_200_iterations_reclaims_the_argument) {
    const std::string src =
        "module M; import System; "
        "type F := () -> int; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public method Get(q: F): F "
        "    return lambda() => 7; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      var f := fl.Get(lambda() => 5); "
        "      t := t + f(); "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// THE BOUNDARY CONTROL — the DEFERRED-ACTIVATION twin (§28.14 *Call-bounded
// × deferred activation*, shape 1 "Frame residence"): a PLAIN `async` callee
// holds the call-bounded callback in its TASK FRAME, and the frame takes its
// OWN +1 on the closure cell (dropped at the task's terminal edge).  The
// caller's +1 is still the caller's to drop, but the frame's hold means the
// callback — and the cells its env points at — must OUTLIVE the call.
//
// This row is GREEN on the base and GREEN with the fix; it is the control
// that a wrong fix — one that releases the callback for a deferred-activation
// callee, or that consumes the captured cells under a frame that still holds
// the closure — turns RED (a double free / a use-after-free under ASan).
BEZEL_TEST(call_bounded_closure_arg_function_typed_result_9174,
           async_callee_holds_the_callback_in_its_task_frame) {
    const std::string src =
        "module M; import System; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public async method Get(q: () -> int): Task<int> "
        "    return q(); "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static async method Main(): Task<int> "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      var v := await fl.Get(lambda() => 5); "
        "      t := t + v; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1000\n");
}

}  // namespace
