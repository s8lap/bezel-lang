// Regression test for issue #9082 — a chain call whose result is OWNED and
// BOUND to a local disposed that result TWICE (an ASan heap-use-after-free in
// `main`).
//
// THE PROGRAM (the issue's own repro, reduced to its loop form):
//
//   module M;
//   public class W public V: int := 3; end W;
//   public class C
//     public static method Make(): () -> owned W
//       return lambda(): W => new W();
//     end Make;
//     public static method Main(): int
//       var w := C.Make()();      // the chain call, BOUND to a local
//       return w.V;
//     end Main;
//   end C;
//
// THE DEFECT. The chain-call result is disposed TWICE:
//
//   * once by the LOCAL's scope-end disposal (`lowerLocalDecl`'s auto-drop,
//     armed because the ownership pass's verdict says the slot is `owned`),
//     and
//   * once by the CG60 chain-call arm's own scope-end disposal, registered on
//     the arm's TEMP slot (`ccres.slot`), which holds the SAME pointer.
//
// `BEZEL_DUMP_IR=1` of `Main` on the base shows both, in that order:
//
//   call.cont8:  %ccres.slot = alloca ptr
//                store ptr %7, ptr %ccres.slot, align 8
//                store ptr %7, ptr %0, align 8        ; the local `w`'s slot
//   using.dispose:      ... free(ptr %13)             ; the local's slot  (FIRST free)
//   using.cls.dispose:  %21 = load ptr, ptr %19       ; ccres.slot — a READ of the
//                        ... free(ptr %19)             ;   object already freed
//
// MEASURED on the base (r30-c1 @ 116823e00), `bezelc --sanitize=address`:
//
//   ERROR: AddressSanitizer: heap-use-after-free on address 0x…0010
//   READ of size 8 at 0x…0010 thread T0
//       #0 … in main
//   freed by thread T0 here: free … in main
//   previously allocated by thread T0 here: … in lam$0 bezel
//
// The same program compiled WITHOUT the sanitizer dies with SIGSEGV (the second
// disposal loads the vtable pointer out of the freed block and calls it), so
// the plain `runAot` oracle is red too.
//
// THE SPEC.
//   §28.3.4 *Producers* (v0.359), the "Any call" row: a call is classified by
//   the announcement table — "`owned T` → **Owned** (row 1)".
//   §28.3.5 *The announcement table* (v0.359), row 1 (*`owned T` over a
//   non-mint-announcing type*), *Function-value invocation* column: **Owned**.
//   (`Make(): () -> owned W` is a function value whose DECLARED return slot
//   carries the `owned` marker, so the invocation's result is the caller's to
//   dispose. Row 4 — a plain concrete class/interface slot, unmarked — is
//   **Borrow**: "The result is never owned".)
//   §28.3.11 *Unbound owned temporaries* (v0.359): "A temporary that is
//   instead **bound** (to a `var`/`using var`), `move`d, returned, or
//   transferred into an owning location is governed by the corresponding
//   transfer or discharge rule and **is not disposed under this rule (it would
//   otherwise be disposed twice)**."
//   §28.3.10 *Owned-local discharge requirement* (v0.359): "An owned local is
//   disposed exactly once on every control-flow path" — explicitly by the
//   program, or by the compiler's affine auto-drop at scope end. "The
//   obligation is always satisfied — by the program or by the compiler."
//
// ⇒ A BOUND chain-call result is disposed by the LOCAL, exactly once. The
// unbound temporary (the third row below) is disposed by §28.3.11, and the
// bound-form twin (the callee is an identifier, so it never reaches the
// chain-call arm) is disposed by the local as well.
//
// THE FIX (src/codegen/LlvmBackend_Calls2.cpp). The CG60 chain-call arm
// registers no scope-end disposal for a result that is BOUND to a local: the
// local's own regime (§28.3.10) is the single disposer. `lowerLocalDecl`
// tells the arm which call is the initializer by arming
// `chainCallResultHandedOff_` with that CALL NODE (#9138 generalised #9082's
// boolean to the node, shared with the other hand-off positions), so only the
// initializer's own chain call is handed off — a chain call nested in an
// ARGUMENT position is a different node, is not bound to a local, and keeps
// §28.3.11's temporary disposal.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// over the GENERATED module and LeakSanitizer at exit. The rows LOOP (200
// iterations inside the program), so neither the use-after-free nor a leak can
// hide under the sanitizer's noise, and the leak leg is what proves the value
// is still disposed EXACTLY ONCE after the fix (a fix that simply stopped
// disposing the value would pass a use-after-free check and fail this one).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// The plain (non-sanitizer) oracle: the program ran, exited with its own
// code, and printed the expected output. `runAot` reports a program that died
// from a signal as `ran == false`.
void expectRan(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The class the chain call mints: a field read AFTER the chain call, so the
// row observes the object the double free corrupted.
const char* kClass =
    "public class W "
    "  public V: int := 3; "
    "end W; ";

// The issue's OWN program, verbatim in shape: one chain call, the result
// bound to a local, the field read as the method's return value (so the
// program's own exit code is the field's value).
std::string literalReproProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  public static method Make(): () -> owned W "
        "    return lambda(): W => new W(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var w := C.Make()(); "
        "    return w.V; "
        "  end Main; "
        "end C;";
}

// The same shape, looped 200 times: the sum (600 = 200 × 3) proves every read
// returned the right value, and 200 objects amplify the report so neither the
// use-after-free nor a leak can hide under the sanitizer's noise.
std::string boundLoopProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  public static method Make(): () -> owned W "
        "    return lambda(): W => new W(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var w := C.Make()(); "
        "      t := t + w.V; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// The UNBOUND twin of the row above: the same owned chain-call result, used
// as a temporary instead of bound to a local. §28.3.11 disposes it at the end
// of its full expression, and the CG60 arm's own scope-end disposal is that
// disposal — so this row must stay GREEN after the fix.
//
// It is the suite's ONE boundary control (`docs/ops/lane-contract.md` point 7
// allows at most two, each naming the plausible wrong fix it is red under):
// it is red under a fix that drops the CG60 arm's registration for EVERY owned
// chain-call result, not just a bound one — 200 minted frames would leak.
std::string unboundLoopProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  public static method Make(): () -> owned W "
        "    return lambda(): W => new W(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      t := t + C.Make()().V; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ROW 1 — the issue's literal repro, on the plain AOT path.
//
// RED on the base: the program dies. Without the sanitizer the second
// disposal loads the vtable pointer out of the freed block and calls it
// (SIGSEGV, exit 139); under `BEZEL_AOT_FORCE_SANITIZE=address` it is the
// ASan heap-use-after-free of the issue.
BEZEL_TEST(owned_chain_call_bound_local_double_free_9082,
           literal_repro_program_runs_and_prints_the_field_value) {
    JitResult r = runAot(literalReproProgram());
    expectRan(r, 3, "");
}

// ROW 2 — the same shape, looped 200 times, on the plain AOT path: the sum
// proves every read returned the right value.
//
// RED on the base: the program dies before it can print the sum.
BEZEL_TEST(owned_chain_call_bound_local_double_free_9082,
           bound_local_loop_prints_the_sum) {
    JitResult r = runAot(boundLoopProgram());
    expectRan(r, 0, "600\n");
}

// ROW 3 — the same loop on the CHECKED path (ASan over the generated module +
// LSan at exit). This is the row that proves the value is still disposed
// EXACTLY ONCE after the fix: the use-after-free is gone AND LeakSanitizer
// counts nothing (a fix that simply stopped disposing the value would fail
// this row with 200 leaked frames).
//
// RED on the base: the ASan heap-use-after-free of the issue.
BEZEL_TEST(owned_chain_call_bound_local_double_free_9082,
           bound_local_loop_disposes_the_result_exactly_once) {
    JitResult r = runAotChecked(boundLoopProgram());
    expectClean(r, 0, "600\n");
}

// ROW 4 — the UNBOUND twin (the boundary control described above): an owned
// chain-call result used as a temporary is still disposed, exactly once.
//
// GREEN on the base and GREEN after the fix.
BEZEL_TEST(owned_chain_call_bound_local_double_free_9082,
           unbound_temporary_result_is_still_disposed_exactly_once) {
    JitResult r = runAotChecked(unboundLoopProgram());
    expectClean(r, 0, "600\n");
}
