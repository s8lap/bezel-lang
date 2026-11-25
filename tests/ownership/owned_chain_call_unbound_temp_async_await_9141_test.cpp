// Regression test for issue #9141 — an UNBOUND owned chain-call result in an
// `async` body that then AWAITS made the compiler emit malformed IR
// ("Instruction does not dominate all uses!") and reject the module.
//
// THE PROGRAM (the issue's own repro, verbatim):
//
//   module M;
//   public class W public V: int := 3; end W;
//   public class C
//     public static method Make(): () -> owned W
//       return lambda(): W => new W();
//     end Make;
//     public static async method Step(): Task<int>
//       var n := C.Make()().V;      // the UNBOUND owned chain-call result
//       await Task.Delay(0);        // the suspension point
//       return n;
//     end Step;
//     public static async method Main(): Task<int>
//       var t := 0;
//       for var j in 0..<200 do
//         t := t + await C.Step();
//       end;
//       Console.WriteLine(t);
//       return 0;
//     end Main;
//   end C;
//
// THE DEFECT. The CG60 chain-call arm (src/codegen/LlvmBackend_Calls2.cpp,
// `lowerCallDispatch`) registers §28.3.11's disposal for the UNBOUND owned
// temporary on a slot it creates with `builder_.CreateAlloca` — at the
// CURRENT insertion point, i.e. inside the state block the chain call lowers
// into. In an `async` body the scope-end disposal is emitted at the END of the
// method body, which is a LATER state block (the one the `await` resumes
// into), and the alloca does not dominate it.
//
// MEASURED on the base (r30-c1 @ 58063e63f), `bezelc` on the program above:
//
//   Instruction does not dominate all uses!
//     %ccres.slot = alloca ptr, align 8
//     %32 = load ptr, ptr %ccres.slot, align 8
//   Instruction does not dominate all uses!
//     %ccres.slot = alloca ptr, align 8
//     %34 = load ptr, ptr %ccres.slot, align 8
//   Instruction does not dominate all uses!
//     %ccres.slot = alloca ptr, align 8
//     store ptr null, ptr %ccres.slot, align 8
//   Instruction does not dominate all uses!
//     %ccres.slot = alloca ptr, align 8
//     store ptr null, ptr %ccres.slot, align 8
//   bezelc: codegen error: IR verification failed (malformed IR emitted; see
//   report above)
//
// (four offenders: the disposal's load of the slot and its nulling store, in
// the two state blocks the two `await`s resume into.)
//
// THE SPEC.
//   §28.3.11 *Unbound owned temporaries* (v0.359): "Such a temporary is
//   disposed at the **end of the full expression** in which it is produced,
//   after every use of it within that expression has completed" — and of a
//   BOUND one: "it is governed by the corresponding transfer or discharge rule
//   and is not disposed under this rule (it would otherwise be disposed twice)".
//   §28.3.10 *Owned-local discharge requirement* (v0.359): an owned value is
//   disposed exactly once on every control-flow path.
//   §30.1 *`async` methods* / §30.2 / §30.13 (v0.359): an `async` body is a
//   state machine; a value live across a suspension point must round-trip
//   through the task's frame, because the resumed activation runs on a fresh
//   stack.
//
// ⇒ The UNBOUND owned chain-call result is an owned temporary that the
// compiler must dispose exactly once. The compiler may implement that
// disposal at the end of the full expression or later, but the slot it reads
// must be a valid one ACROSS the suspension: the module must verify, and the
// object must be reclaimed exactly once.
//
// THE FIX (src/codegen/LlvmBackend_Calls2.cpp). In an `async` resume body the
// slot is an ENTRY-BLOCK alloca and is registered in
// `asyncHandlerSpillSlots_`, so `promoteDemotedSlotsToFrame` moves it into the
// frame's heap-spill area: the GEP that replaces it is computed in the resume
// entry (so it dominates every state block) and the pointer SURVIVES a real
// cross-thread suspend (a plain stack slot would be garbage on the worker's
// fresh stack). The spill area is `calloc`'d, so a path that reaches the
// disposal without the store reads null and the null-guarded disposal skips.
//
// The SYNCHRONOUS twin keeps the base's `builder_.CreateAlloca` byte for byte
// — a sync body's disposal is emitted in a block the current alloca dominates,
// so the sync shape never needed the entry-block slot.
//
// ORACLE. `runAot` (the plain AOT path) and `runAotChecked` (the checked AOT
// path with AddressSanitizer over the GENERATED module and LeakSanitizer at
// exit). The rows LOOP (200 iterations inside the program), so a leak cannot
// hide under the sanitizer's noise.

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

// The plain (non-sanitizer) oracle: the program compiled, ran, exited with
// its own code, and printed the expected output. `runAot` reports a program
// that died from a signal as `ran == false`, and a module the compiler
// REJECTED (the base's IR-verification failure) as `ran == false` too.
void expectRan(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The class the chain call mints: a field read AFTER the chain call, so the
// rows observe the object the malformed IR's disposal would have touched.
const char* kClass =
    "public class W "
    "  public V: int := 3; "
    "end W; ";

// The issue's OWN program, verbatim in shape: an UNBOUND owned chain-call
// result, an `await` after it, and the field read as the awaited result.
std::string literalReproProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  public static method Make(): () -> owned W "
        "    return lambda(): W => new W(); "
        "  end Make; "
        "  public static async method Step(): Task<int> "
        "    var n := C.Make()().V; "
        "    await Task.Delay(0); "
        "    return n; "
        "  end Step; "
        "  public static async method Main(): Task<int> "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      t := t + await C.Step(); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// The SYNCHRONOUS twin of the program above: the same UNBOUND owned chain-call
// result, the same 200-iteration loop, with no `await` in between. This is
// the suite's ONE boundary control (`docs/ops/lane-contract.md` point 7
// allows at most two, each naming the plausible wrong fix it is red under):
// it is red under a fix that drops the CG60 arm's §28.3.11 registration for
// EVERY owned chain-call result — 200 minted objects would leak — and it
// pins that the fix left the synchronous path byte-identical.
std::string syncTwinProgram() {
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
// RED on the base: the compiler REJECTS the module with "IR verification
// failed (malformed IR emitted)" — the program never runs, so `ran` is false
// and the expected output is missing.
BEZEL_TEST(owned_chain_call_unbound_temp_async_await_9141,
           literal_repro_program_compiles_and_runs) {
    JitResult r = runAot(literalReproProgram());
    expectRan(r, 0, "600\n");
}

// ROW 2 — the same program on the CHECKED path (ASan over the generated
// module + LSan at exit). This is the row that proves the value is disposed
// EXACTLY ONCE after the fix: the temporary survives the suspension point
// (no malformed IR), and LeakSanitizer counts nothing — a fix that stopped
// disposing the value, or disposed it twice, would fail this row.
//
// RED on the base: the module is rejected before it can run.
BEZEL_TEST(owned_chain_call_unbound_temp_async_await_9141,
           unbound_temp_survives_the_suspension_and_is_disposed_exactly_once) {
    JitResult r = runAotChecked(literalReproProgram());
    expectClean(r, 0, "600\n");
}

// ROW 3 — the SYNCHRONOUS twin (the boundary control described above): the
// same owned chain-call result used as a temporary in a plain method is still
// disposed, exactly once, and still prints the same sum.
//
// GREEN on the base and GREEN after the fix.
BEZEL_TEST(owned_chain_call_unbound_temp_async_await_9141,
           sync_twin_still_disposes_the_unbound_temp_exactly_once) {
    JitResult r = runAotChecked(syncTwinProgram());
    expectClean(r, 0, "600\n");
}
