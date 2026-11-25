// Regression test for issue #9126 — an `int` capture cell of an ASYNC or
// ITERATOR method's PARAMETER, captured by a closure that escapes the frame
// (stored into a field of a caller-held object), leaked 4 bytes per call.
//
// THE DEFECT (MEASURED on the base `r30-c1` @ 65a4a8e19, `bezelc
// --sanitize=address`, the issue's own repro, 200 iterations):
//
//   ==ERROR: LeakSanitizer: detected memory leaks
//   Direct leak of 800 byte(s) in 200 object(s) allocated from:
//       #0 0x… in calloc
//       #1 0x… in bz$async$M.C$Fill$Resume bezel
//   SUMMARY: AddressSanitizer: 800 byte(s) leaked in 200 allocation(s).
//
// 800 B = 4 B x 200: the heap capture cell of the method's `int` PARAMETER
// `k` (§27.5: "Captured variables are heap-allocated in a closure object").
// It was allocated by the RESUME frame and freed by NOBODY.
//
// WHY NOBODY FREED IT. A captured parameter has no `declScopeDepth_` entry,
// so the frame-side reclaim of its capture cell has no scope to go in — that
// is what #7611's `cellOwnerScope_` map is for: it gives a captured
// PARAMETER the ROOT scope of the body that binds it (index 0), and the
// declaring frame then drops its own reference to the cell there with
// `bz_cell_release` (#4509's COUNTING protocol: `bz_cell_retain` at the
// capture-store, `bz_cell_release` in the env's envdtor, `bz_cell_release`
// once at the declaring scope's end — "the last one out frees the cell").
//
// #7611 DECLINED that map for an async or iterator state machine
// (`!smStateStruct_`), on the note "the parameter or for-var lives in the
// frame struct, not a cell". That is the PARAMETER's own storage, not the
// CAPTURE CELL's: a captured parameter in a state-machine body IS boxed
// into a heap cell exactly as in a synchronous body (#3466 — lowerLambda
// maps the fresh cell-HOLDER alloca to a frame field so the holder survives
// every suspension), and the capturing env points at that cell.
//
// With the owner scope declined, `cellOwnerScopeIndex` fails, so the
// COUNTING protocol is never armed for the parameter: no
// `bz_cell_retain` at the capture-store, no `bz_cell_release` in the env's
// envdtor, and — the actual leak — no `bz_cell_release` in the declaring
// frame. The cell was allocated by the resume / moveNext frame and freed by
// nobody. MEASURED on the binary: the async repro contains NO
// `bz_cell_retain` / `bz_cell_release` call at all, while its synchronous
// twin contains exactly one of each.
//
// THE SPEC (v0.359).
//   §27.5 "Closures": "Captured variables are heap-allocated in a closure
//   object, a `ref` or `out` parameter excepted."
//   §28.14, arm 1: "a captured local or parameter of a value type that holds
//   no borrows … lives in its heap capture cell, which the closure keeps
//   alive".
//   §28.3, closure bullet, provenance clause 1: "a captured value that
//   carries no borrow — a value type that holds none … — contributes
//   nothing, and lives in its heap capture cell".
//   §28.9 "Runtime-managed types": "Runtime management governs the
//   *function value itself* — the heap-allocated closure object that backs
//   it — and relaxes nothing about the values the closure captures".
//
// ⇒ The capture cell is kept alive BY THE CLOSURE; when the last holder
// (the field's closure, then the frame) is gone the cell is freed. A
// 4-byte cell must not outlive the program's last use, and in a state
// machine the frame's half of that accounting was missing.
//
// ORACLE. `runAotChecked` — the checked AOT path with the
// AddressSanitizer/LeakSanitizer pass over the GENERATED module. Every row
// LOOPS 200 times INSIDE the program, so a cell leaked per call is leaked
// 200 times over and the leak count is unambiguous, and each row asserts
// the program's own output and exit code as well as the sanitizer's
// verdict.
//
// ROW 1 is the ISSUE'S LITERAL ASYNC REPRO, verbatim. ROW 2 is its
// ITERATOR twin (`yield return` in place of the `await`). ROW 3 is the
// no-escape CONTROL: the same shape with the closure invoked INSIDE the
// frame and never stored into a field. It is green on the base and on the
// fix — it is the suite's boundary control, and it is the row that catches
// the plausible wrong fix named in the commit body: arming the COUNTING
// protocol for a capture the plain scope-end free already reclaims would
// free the same cell twice, and ROW 3 is where that double free shows.
//
// MEASURED WITH THE FIX (the numbers this file asserts):
//   ROW 1  stdout "20500"  exit 0  ASan: clean  LSan: 0 leaked
//   ROW 2  stdout "21100"  exit 0  ASan: clean  LSan: 0 leaked
//   ROW 3  stdout "20500"  exit 0  ASan: clean  LSan: 0 leaked
// MEASURED ON THE BASE (the rows this file must turn RED):
//   ROW 1  stdout "20500"  exit 1  LSan: 800 B / 200 allocations
//   ROW 2  stdout "21100"  exit 1  LSan: 800 B / 200 allocations
//   ROW 3  stdout "20500"  exit 0  ASan: clean  LSan: 0 leaked (control)

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

// The generated-code AddressSanitizer did NOT fire: no use-after-free, no
// double free, no out-of-bounds read.
void assertNoAddressSanitizerError(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    if (text.find("ERROR: AddressSanitizer") != std::string::npos)
        std::cout << "    [ ASAN REPORT ] " << text << "\n";
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
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
}

// ROW 1 — THE ISSUE'S LITERAL REPRO (the async form), verbatim:
//
//   Fill(box, k)
//     var g: Fn := lambda(x: int, y: int) => 0;
//     var b: Fn := lambda(x: int, y: int) => x - y + k;   // captures `k`
//     box.F := b;                                         // `b` ESCAPES
//     await Task.Delay(0);
//     b := g;
//
// The caller invokes the kept closure after `Fill` returned, so the capture
// cell is read while the closure that captured it is still live — the shape
// §28.14 arm 1 describes ("lives in its heap capture cell, which the
// closure keeps alive").
//
// RED on the base: LeakSanitizer, "800 byte(s) leaked in 200 allocation(s)",
// the 4-byte cell allocated by `bz$async$M.C$Fill$Resume`.
BEZEL_TEST(capture_cell_released_in_async_iterator_frame_9126,
           issue_repro_async_parameter_capture_cell_is_released_with_the_frame) {
    const std::string src =
        "module M;\n"
        "type Fn := (int, int) -> int;\n"
        "public class Box\n"
        "  public F: Fn := lambda(x: int, y: int) => 0;\n"
        "end Box;\n"
        "public class C\n"
        "  public static async method Fill(box: Box, k: int): Task\n"
        "    var g: Fn := lambda(x: int, y: int) => 0;\n"
        "    var b: Fn := lambda(x: int, y: int) => x - y + k;\n"
        "    box.F := b;\n"
        "    await Task.Delay(0);\n"
        "    b := g;\n"
        "  end Fill;\n"
        "  public static async method Main(): Task<int>\n"
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var box: Box := new Box();\n"
        "      await Fill(box, i);\n"
        "      total := total + box.F(5, 2);\n"
        "    end for;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "20500\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

// ROW 2 — THE ITERATOR TWIN of ROW 1 (`yield return` in place of the
// `await`), the second shape the issue names. The capture cell is allocated
// by the ITERATOR's MoveNext frame, and the frame's half of the counting
// protocol was missing there too.
//
// RED on the base: LeakSanitizer, "800 byte(s) leaked in 200 allocation(s)".
BEZEL_TEST(capture_cell_released_in_async_iterator_frame_9126,
           issue_repro_iterator_parameter_capture_cell_is_released_with_the_frame) {
    const std::string src =
        "module M;\n"
        "type Fn := (int, int) -> int;\n"
        "public class Box\n"
        "  public F: Fn := lambda(x: int, y: int) => 0;\n"
        "end Box;\n"
        "public class C\n"
        "  public static method Gen(k: int, box: Box): IEnumerable<int>\n"
        "    var g: Fn := lambda(x: int, y: int) => 0;\n"
        "    var b: Fn := lambda(x: int, y: int) => x - y + k;\n"
        "    box.F := b;\n"
        "    yield return 1;\n"
        "    b := g;\n"
        "    yield return 2;\n"
        "  end Gen;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var box: Box := new Box();\n"
        "      for var v in C.Gen(i, box) do total := total + v; end;\n"
        "      total := total + box.F(5, 2);\n"
        "    end for;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "21100\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

// ROW 3 — THE NO-ESCAPE CONTROL. The same async shape, but the closure is
// invoked INSIDE the frame and never stored into a field, so it does not
// outlive the frame and its capture cell is reclaimed by the frame's own
// scope-end route.
//
// Green on the base and on the fix — it is the suite's ONE boundary
// control. It earns its place by being the row that catches the plausible
// wrong fix named in the commit body: the COUNTING protocol must NOT be
// armed for a capture whose cell the plain scope-end free already
// reclaims, because arming both frees the same cell twice. Under that
// wrong fix ROW 3 reports a double free (an AddressSanitizer error) while
// ROWS 1 and 2 stay green — so this row is what keeps the fix from
// trading a 4-byte leak for a use-after-free.
BEZEL_TEST(capture_cell_released_in_async_iterator_frame_9126,
           control_async_parameter_capture_cell_without_an_escaping_closure) {
    const std::string src =
        "module M;\n"
        "type Fn := (int, int) -> int;\n"
        "public class C\n"
        "  public static async method Fill(k: int): Task<int>\n"
        "    var g: Fn := lambda(x: int, y: int) => 0;\n"
        "    var b: Fn := lambda(x: int, y: int) => x - y + k;\n"
        "    var v := b(5, 2);\n"
        "    b := g;\n"
        "    await Task.Delay(0);\n"
        "    return v;\n"
        "  end Fill;\n"
        "  public static async method Main(): Task<int>\n"
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      total := total + await Fill(i);\n"
        "    end for;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "20500\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

}  // namespace
