// Regression test for issue #9099 — a lambda index argument whose body
// indexes with a lambda (`fl[lambda() => fl[lambda() => ..]]`) made the
// compiler ABORT: "Instruction does not dominate all uses!".
//
// THE DEFECT. `public virtual property [q: () -> int]: int => q();` — a
// class-declared VIRTUAL indexer whose parameter is a plain function-typed
// parameter — read through a `new Fl()` receiver, with the index argument a
// LAMBDA LITERAL that CAPTURES THE RECEIVER. MEASURED on r30-c1 @
// ff0cc3c53 (`scripts/agent_check.sh --probe`), the compiler aborts:
//
//   Instruction does not dominate all uses!
//     %cell7 = load ptr, ptr %2, align 8
//     %72 = call i32 %idxget(ptr %cell7, ptr %61)
//   bezelc: codegen error: IR verification failed (malformed IR emitted; ...)
//
// A compiler abort is never a valid outcome: §23.4 (v0.359) makes an indexer
// read an ordinary property read whose index arguments are ordinary
// expressions, and §27.5 (v0.359) makes a lambda's capture of an enclosing
// local an ordinary capture. The program is a legal program and must compile
// and run.
//
// THE ROOT CAUSE. The receiver `fl` is CAPTURED by the index argument's
// lambda, so the capture site BOXES it into a capture cell (§27.5: "Captured
// variables are heap-allocated in a closure object"). Fix #9004's
// `Emitter::redirectSlotToCellHolder` (src/codegen/LlvmBackend_Layout.cpp)
// then REWRITES the accesses the enclosing body already emitted against the
// variable's pre-boxing alloca, so they address the cell — §27.5's "A captured
// variable therefore has one source set at every read, inside or outside the
// lambda". It did so by building a replacement load and ERASING the original:
//
//   llvm::Value* redirected = builder_.CreateLoad(li->getType(), addr, ...);
//   li->replaceAllUsesWith(redirected);
//   li->eraseFromParent();
//
// `replaceAllUsesWith` reaches the load's LLVM OPERAND edges. It does not
// reach a `llvm::Value*` held in a C++ container — and
// `Emitter::lowerIndexerGet` (src/codegen/LlvmBackend_Assign.cpp) holds the
// receiver in exactly such a container: it pushes the receiver into
// `callArgs` BEFORE it lowers the index arguments, so that the receiver is
// evaluated before them (§23.4's index read). Lowering a lambda index
// argument performs the capture, hence the boxing, hence the erase — and the
// erased instruction's address went back to LLVM's slab allocator, where the
// NEXT instruction of the same size took it. The call was then built on that
// instruction, which is a STRANGER than the receiver the vtable slot was
// loaded from. Two measured manifestations, both on r30-c1 @ ff0cc3c53:
//   (a) the stranger lived in a block that does NOT dominate the call — the
//       module failed the verifier ("Instruction does not dominate all uses!"),
//       the compiler ABORTED and the program never ran;
//   (b) the stranger lived in a dominating block — the module verified, but
//       the RECEIVER ARGUMENT was the capture CELL's pointer instead of the
//       receiver object, so the indexer read the wrong object (or read past
//       the cell). Row 2 below is that manifestation.
//
// THE FIX. The redirect now REPOINTS the load in place (the STORE and GEP
// arms of the same function already did) instead of erasing it and RAUW-ing a
// replacement in. The instruction is the same — a load of the same type from
// the same address, in the same place — so no behaviour changes; only the
// load's IDENTITY survives, and no holder of it is left dangling.
//
// THE SPEC. §23.4 (v0.359) *Indexers*: an indexer is a property whose name is
// replaced by `[parameters]`, and the index arguments are ordinary
// expressions. §27.5 (v0.359) *Closures*: "Captured variables are
// heap-allocated in a closure object ... Modifications to a captured variable
// inside the lambda are visible outside, and vice versa. A captured variable
// therefore has one source set at every read, inside or outside the lambda"
// — the sentence `redirectSlotToCellHolder` implements, and the one the fix
// keeps implementing. §28.3 (v0.359) *Borrow lifetime rules*' closure bullet
// is the same rule seen from the ownership side. Nothing in the spec turns on
// the receiver being captured, so the program is admitted and the compiler's
// abort was a defect.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing. The rows
// LOOP (200 iterations inside the program), so a per-iteration leak cannot
// hide under the sanitizer's noise.
//
// ROWS (both RED on the base, both GREEN with the fix; the spec-decided
// GREEN twins are left to the conformance corpus — see the handback).
//   Row 1 — the issue's own repro: a VIRTUAL indexer, a lambda index
//           argument whose body indexes with a lambda, 200 iterations. RED on
//           the base (the compiler ABORTS: the module fails the verifier),
//           GREEN with the fix (prints 8400 = 200 x 42).
//   Row 2 — the RECEIVER IDENTITY: the same shape, but the indexer's body
//           reads the receiver's own field (`q() + Tag`), so the receiver
//           argument MATTERS. On the base the receiver argument is the
//           capture CELL's pointer, so the getter reads `Tag` one word past
//           the cell — MEASURED on r30-c1: the program SEGFAULTS (exit 139,
//           three runs, three times), and under the sanitizer it is a
//           heap-buffer-overflow. With the fix the receiver is the object and
//           the program prints 40000 = 200 x (100 + 100). This row is what
//           pins the fix as a SEMANTIC fix and not merely a crash fix: a fix
//           that only silenced the verifier error (keeping the stranger
//           receiver) leaves it red.
//
// per §23.4, §27.5, §28.3 (issue #9099, v0.359)

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

// The shared preamble: a class instance whose field the inner lambda
// captures.
constexpr const char* kRes =
    "module M; "
    "public class Res "
    "  public V: int; "
    "  public constructor() V := 42; end constructor; "
    "end Res; ";

// ── Row 1 (the issue's own repro / §23.4 + §27.5): a VIRTUAL indexer on a
// CLASS receiver, invoked with an inline lambda whose body indexes with a
// second lambda. 200 iterations.
//
// RED on the base: the compiler ABORTS ("Instruction does not dominate all
// uses!"), so the module never reaches the linker and the row fails.
// GREEN with the fix: the module verifies, the program runs, and it prints
// 8400 = 200 x 42 — one 42 per iteration, so the inner index really ran and
// really returned the captured field.
BEZEL_TEST(lambda_index_arg_receiver_capture_9099,
           issue_repro_nested_lambda_index_argument_compiles_and_runs) {
    const std::string src = std::string(kRes) +
        "public class Fl "
        "  public virtual property [q: () -> int]: int => q(); "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl := new Fl(); "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      t := t + fl[lambda() => fl[lambda() => r.V]]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "8400\n");
}

// ── Row 2 (the RECEIVER IDENTITY / §27.5's "one source set at every read"):
// the same shape — a VIRTUAL indexer, a lambda index argument that CAPTURES
// THE RECEIVER — but the indexer's body reads the receiver's own field
// (`q() + Tag`), so the receiver ARGUMENT matters: with the right receiver
// the getter returns 100 + 100 = 200 per iteration.
//
// RED on the base: the receiver argument is the capture CELL's pointer rather
// than the receiver object, so the getter reads `Tag` one word past the cell.
// MEASURED on r30-c1 @ ff0cc3c53: the program SEGFAULTS (exit 139, three
// runs, three times); under the sanitizer the row reports the out-of-bounds
// read. GREEN with the fix: the receiver is the object and the program
// prints 40000 = 200 x 200.
//
// This row is what pins the fix as a SEMANTIC fix and not merely a crash
// fix: a fix that only silenced the verifier error — keeping the stranger
// receiver — leaves it red.
BEZEL_TEST(lambda_index_arg_receiver_capture_9099,
           index_argument_capturing_the_receiver_keeps_the_receiver_identity) {
    const std::string src =
        "module M; "
        "public class Fl "
        "  public virtual property [q: () -> int]: int => q() + Tag; "
        "  public Tag: int; "
        "  public constructor() Tag := 100; end constructor; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl := new Fl(); "
        "    for var j in 0..<200 do "
        "      t := t + fl[lambda() => fl.Tag]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "40000\n");
}

}  // namespace
