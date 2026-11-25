// Regression test for issue #9166 — a lambda literal that captures a local
// of an ITERATOR method and is invoked inside a `yield return` OPERAND
// (`yield return (lambda() => q + 1)();`) leaked its §27.5 capture cell once
// per evaluation.
//
// THE DEFECT (MEASURED on the base `r30-c1` @ 62d96b17b, `bezelc
// --sanitize=address`, the issue's own repro, 200 yields):
//
//   S=20100
//   Direct leak of 800 byte(s) in 200 object(s) allocated from:
//       #0 0x… in calloc
//       #1 0x… in bz$iter$M.It$Each$MoveNext bezel
//   SUMMARY: AddressSanitizer: 800 byte(s) leaked in 200 allocation(s).
//
// 800 B = 4 B x 200: the heap capture cell of the iterator's local `q`
// (§27.5: "Captured variables are heap-allocated in a closure object"). It
// was allocated by the iterator's MoveNext frame and freed by NOBODY.
//
// WHY NOBODY FREED IT — TWO HALVES, BOTH IN THE YIELD OPERAND'S PATH.
//
// (1) The capture-cell CLASSIFICATION never saw the yield operand.
//     `collectCellFreeEligible` (LlvmBackend_Setup.cpp) walks the body and
//     classifies every captured local: a local captured only by
//     NON-escaping closures qualifies for the scope-end `boxedCellFree`
//     that reclaims its cell. Its walker had NO `ast::YieldStmt` arm, so a
//     lambda literal written in a `yield return` operand was invisible: the
//     IIFE there never reached `capturedByNonEscaping`, the captured local
//     never entered `cellFreeEligible_`, and the scope-end free was never
//     armed. The issue's own control — the SAME lambda in a local
//     initializer (`var v := (lambda() => q + 1)(); yield return v;`) — is
//     walked by the `ast::LocalDecl` arm and is clean on the base.
//
// (2) The yield handler's suspend-local SPILL ran BEFORE the operand was
//     evaluated, so the frame never carried the cell HOLDER that the
//     operand's own capture creates. Boxing a local RE-MAPS its frame
//     field: the cell holder takes the local's own (ptr) field and the
//     local's pre-boxing slot takes the frame's separate PRE-BOX field
//     (boxLocalSlot, LlvmBackend_Exprs2.cpp). The pre-operand spill wrote
//     the pre-boxing VALUE into the holder's ptr field and never wrote the
//     holder, so across the suspension the holder held a truncated integer
//     where the cell pointer belongs. MEASURED on the base, in the sibling
//     shape that READS the captured local after the resume
//     (`yield return (lambda() => q + 1)(); q := q + 1;`): a SEGV at the
//     garbage address. With half (1) alone the same garbage was FREED by
//     the scope-end cell free (`free` of a non-pointer — a SEGV under ASan).
//
// THE ISSUE NAMES TWO SHAPES. This file covers both: ROWS 1-3 are the
// integer capture (`yield return (lambda() => q + 1)();`), and ROW 6 is the
// issue's second shape — "With an owning-match case expression as the
// operand (`yield return (case h.Pick(k) of Text(r) =>
// (lambda() => r.Id())(), …)`) the same cell leaks (8 bytes x 201) while the
// payloads are correctly disposed".
//
// THE SPEC (v0.359).
//   §27.5 "Closures": "Captured variables are heap-allocated in a closure
//   object, a `ref` or `out` parameter excepted." … "Captures share one
//   cell" (§28.3).
//   §28.9 "Runtime-managed types": the closure object is runtime-managed
//   and "the compiler tracks captured borrows separately from the function
//   value's own lifetime" — but a capture CELL is the closure's own
//   storage, and the compiler reclaims it when the last holder is gone.
//   §28.3 *Local variables, ownership and borrows*, closure bullet,
//   provenance clause 1: a captured value that carries no borrow "lives in
//   its heap capture cell" — the cell belongs to the CAPTURING scope, so
//   the scope that captures it reclaims it.
//   §29.3 "Iterator methods": "`yield return expression;` produces the next
//   value and suspends" — the operand is a full expression, evaluated
//   BEFORE the suspension, exactly as the local initializer's expression
//   is. §29.3's state machine ("the compiler translates the body into the
//   equivalent state-machine implementation") must therefore carry the
//   operand's captures across the suspension like every other suspend-local.
//
// ⇒ A lambda literal in a `yield return` operand is a lambda literal like
// any other: its capture cell is reclaimed when the capturing scope ends,
// and the state machine carries the capture across the suspension. The
// local-initializer twin the issue names is the control that says so.
//
// ORACLE. `runAotChecked` — the checked AOT path with the
// AddressSanitizer/LeakSanitizer pass over the GENERATED module. Every row
// LOOPS 200 times INSIDE the program, so a cell leaked per evaluation is
// leaked 200 times over and the leak count is unambiguous, and each row
// asserts the program's own output and exit code as well as the
// sanitizer's verdict.
//
// ROWS 1-3 and ROW 6 are RED on the base (LeakSanitizer: 800 byte(s) in
// 200 allocations for ROWS 1-3; 1608 byte(s) in 201 allocations for ROW 6);
// ROWS 4-5 are the suite's two boundary controls — green on
// the base and on the fix. ROW 4 is the issue's own local-initializer twin
// (the control the issue names) and ROW 5 is the captured-closure CALL
// (`yield return f();` with `f` a local) — the row that catches the
// plausible wrong fix named in the commit body: releasing the capture cell
// at the yield-operand site instead of at the capturing scope's end would
// free the cell of a closure that is still live (ROW 5), and arming the
// scope-end free for a local that an ESCAPING closure also captures would
// double-free it.
//
// MEASURED WITH THE FIX (the numbers this file asserts):
//   ROW 1  stdout "S=20100"  exit 0  ASan: clean  LSan: 0 leaked
//   ROW 2  stdout "S=20100"  exit 0  ASan: clean  LSan: 0 leaked
//   ROW 3  stdout "S=20100"  exit 0  ASan: clean  LSan: 0 leaked
//   ROW 4  stdout "S=20100"  exit 0  ASan: clean  LSan: 0 leaked (control)
//   ROW 5  stdout "S=20100"  exit 0  ASan: clean  LSan: 0 leaked (control)
//   ROW 6  stdout "N=201 Made=201 S=1605"  exit 0  ASan: clean  LSan: 0
// MEASURED ON THE BASE (the rows this file must turn RED):
//   ROW 1  stdout "S=20100"  exit 1  LSan: 800 B / 200 allocations
//   ROW 2  stdout "S=20100"  exit 1  LSan: 800 B / 200 allocations
//   ROW 3  stdout "S=20100"  exit 1  LSan: 800 B / 200 allocations
//   ROW 4  stdout "S=20100"  exit 0  ASan: clean  LSan: 0 leaked (control)
//   ROW 5  stdout "S=20100"  exit 0  ASan: clean  LSan: 0 leaked (control)
//   ROW 6  stdout "N=201 Made=201 S=1605"  exit 1  LSan: 1608 B / 201 allocs

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

// The iterator's own source — shared by the rows below so that the only
// difference between a RED row and a control is the shape the row names.
const char* kSyncIterator =
    "module M;\n"
    "import System.Collections.Generic;\n"
    "public class It\n"
    "  public static method Each(n: int): IEnumerable<int>\n"
    "    var k := 0;\n"
    "    while k < n do\n"
    "      var q := k;\n"
    // The operand under test — the rows below substitute their own.
    "      yield return (lambda() => q + 1)();\n"
    "      k := k + 1;\n"
    "    end while;\n"
    "  end Each;\n"
    "end It;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := 0;\n"
    "    for var x in It.Each(200) do s := s + x; end for;\n"
    "    Console.WriteLine($\"S={s}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// ── ROW 1 — THE ISSUE'S LITERAL REPRO ──────────────────────────────────────
// A lambda literal that captures the iterator's local `q` and is invoked
// INSIDE the `yield return` operand. 200 yields, so the cell leaks 800
// bytes on the base.
//
// RED on the base: LeakSanitizer, "800 byte(s) leaked in 200 allocation(s)",
// the 4-byte cell allocated by `bz$iter$M.It$Each$MoveNext`.
BEZEL_TEST(iterator_yield_operand_lambda_capture_cell_9166,
           issue_repro_iife_in_yield_operand_does_not_leak_its_capture_cell) {
    const JitResult r = runAotChecked(kSyncIterator);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // false on a leak or a crash
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "S=20100\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

// ── ROW 2 — THE NESTED-LAMBDA VARIANT ─────────────────────────────────────
// The inner lambda captures `q` and the outer lambda (also in the operand)
// invokes it. Two cells per evaluation would leak 1600 bytes on the base;
// the outer lambda's own capture of nothing adds nothing.
//
// RED on the base: LeakSanitizer, "800 byte(s) leaked in 200 allocation(s)".
BEZEL_TEST(iterator_yield_operand_lambda_capture_cell_9166,
           nested_lambda_in_yield_operand_does_not_leak_its_capture_cell) {
    const std::string src =
        "module M;\n"
        "import System.Collections.Generic;\n"
        "public class It\n"
        "  public static method Each(n: int): IEnumerable<int>\n"
        "    var k := 0;\n"
        "    while k < n do\n"
        "      var q := k;\n"
        "      yield return (lambda() => (lambda() => q + 1)())();\n"
        "      k := k + 1;\n"
        "    end while;\n"
        "  end Each;\n"
        "end It;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    for var x in It.Each(200) do s := s + x; end for;\n"
        "    Console.WriteLine($\"S={s}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "S=20100\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

// ── ROW 3 — THE ASYNC-ITERATOR TWIN ────────────────────────────────────────
// §30.7's async iterator is the same state machine with the same
// suspend-local machinery, so the same defect and the same fix apply.
//
// RED on the base: LeakSanitizer, "800 byte(s) leaked in 200 allocation(s)",
// the cell allocated by the async iterator's resume frame.
BEZEL_TEST(iterator_yield_operand_lambda_capture_cell_9166,
           async_iterator_iife_in_yield_operand_does_not_leak_its_capture_cell) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "import System.Collections.Generic;\n"
        "public class It\n"
        "  public static async method Each(n: int): IAsyncEnumerable<int>\n"
        "    var k := 0;\n"
        "    while k < n do\n"
        "      var q := k;\n"
        "      yield return (lambda() => q + 1)();\n"
        "      k := k + 1;\n"
        "    end while;\n"
        "  end Each;\n"
        "end It;\n"
        "public class Program\n"
        "  public static async method Main(): Task<int>\n"
        "    var s := 0;\n"
        "    await for var x in It.Each(200) do s := s + x; end;\n"
        "    Console.WriteLine($\"S={s}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "S=20100\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

// ── ROW 4 — CONTROL: the issue's own local-initializer twin ───────────────
// The SAME lambda in a local initializer, its result yielded. The issue
// states this shape does not leak; it is the control that says the two
// shapes must agree, and it is GREEN on the base and on the fix.
//
// It is also the row that catches the plausible wrong fix of releasing the
// cell at the yield site rather than at the capturing scope's end: this
// row's cell is reclaimed at the scope end, and a site-release would free
// it twice (the scope-end free is armed for it).
BEZEL_TEST(iterator_yield_operand_lambda_capture_cell_9166,
           control_local_initializer_twin_stays_leak_free) {
    const std::string src =
        "module M;\n"
        "import System.Collections.Generic;\n"
        "public class It\n"
        "  public static method Each(n: int): IEnumerable<int>\n"
        "    var k := 0;\n"
        "    while k < n do\n"
        "      var q := k;\n"
        "      var v := (lambda() => q + 1)();\n"
        "      yield return v;\n"
        "      k := k + 1;\n"
        "    end while;\n"
        "  end Each;\n"
        "end It;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    for var x in It.Each(200) do s := s + x; end for;\n"
        "    Console.WriteLine($\"S={s}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "S=20100\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

// ── ROW 5 — CONTROL: a captured-closure CALL in the operand ───────────────
// `f` is a local closure over `q`; the operand CALLS it. The closure is
// released at the capturing scope's end and its cell is reclaimed there
// too. GREEN on the base and on the fix.
//
// It is the row that catches the plausible wrong fix of arming the
// scope-end cell free for a local an ESCAPING closure also captures: here
// the closure over `q` escapes the operand's evaluation (it lives in a
// local), and a scope-end free that ignored that would free the cell while
// the closure still points at it.
BEZEL_TEST(iterator_yield_operand_lambda_capture_cell_9166,
           control_captured_closure_call_in_yield_operand_stays_leak_free) {
    const std::string src =
        "module M;\n"
        "import System.Collections.Generic;\n"
        "public class It\n"
        "  public static method Each(n: int): IEnumerable<int>\n"
        "    var k := 0;\n"
        "    while k < n do\n"
        "      var q := k;\n"
        "      var f := lambda() => q + 1;\n"
        "      yield return f();\n"
        "      k := k + 1;\n"
        "    end while;\n"
        "  end Each;\n"
        "end It;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    for var x in It.Each(200) do s := s + x; end for;\n"
        "    Console.WriteLine($\"S={s}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "S=20100\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

// ── ROW 6 — THE ISSUE'S OWNING-MATCH CASE-EXPRESSION OPERAND ──────────────
// The issue names this shape too: "With an owning-match case expression as
// the operand (`yield return (case h.Pick(k) of Text(r) =>
// (lambda() => r.Id())(), …)`) the same cell leaks (8 bytes x 201) while the
// payloads are correctly disposed". The lambda in the `Text` arm captures the
// OWNED pattern binding `r`; the `Two` arm captures two of them.
//
// The count is exact: over k = 0..199 the `Text` arm is taken 67 times (1
// payload each), the `Two` arm 67 times (2 payloads each) and the `Empty` arm
// 66 times, so 201 payloads are created and 201 destructors must run
// ("Made=201 N=201") — the payloads are disposed, and only the CELLS leak.
//
// RED on the base: LeakSanitizer, "1608 byte(s) leaked in 201 allocation(s)"
// (8 B x 201 — the issue's number), the cells allocated by
// `bz$iter$M.It$Each$MoveNext`. Green on the fix: no leak, and the payloads
// are still disposed exactly once ("Made=201 N=201").
BEZEL_TEST(iterator_yield_operand_lambda_capture_cell_9166,
           owning_match_case_expression_operand_does_not_leak_its_capture_cell) {
    const std::string src =
        "module M;\n"
        "import System.Collections.Generic;\n"
        "public class Res\n"
        "  public static N: int := 0;\n"
        "  public static Made: int := 0;\n"
        "  public constructor() Res.Made := Res.Made + 1; end constructor;\n"
        "  destructor Res.N := Res.N + 1; end destructor;\n"
        "  public method Id(): int => 7;\n"
        "end Res;\n"
        "public choice OChoice Text(owned r: Res), Two(owned a: Res, owned b: Res), Empty;\n"
        "end OChoice;\n"
        "public class H\n"
        "  public method Pick(i: int): OChoice\n"
        "    if i mod 3 = 0 then return OChoice.Text(new Res()); end;\n"
        "    if i mod 3 = 1 then return OChoice.Two(new Res(), new Res()); end;\n"
        "    return OChoice.Empty;\n"
        "  end Pick;\n"
        "end H;\n"
        "public class It\n"
        "  public static method Each(h: H, n: int): IEnumerable<int>\n"
        "    var k := 0;\n"
        "    while k < n do\n"
        "      yield return (case h.Pick(k) of Text(r) => (lambda() => r.Id())(),"
        " Two(a, b) => (lambda() => a.Id() + b.Id())(), Empty => 3);\n"
        "      k := k + 1;\n"
        "    end while;\n"
        "  end Each;\n"
        "end It;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var h := new H();\n"
        "    var s := 0;\n"
        "    for var x in It.Each(h, 200) do s := s + x; end for;\n"
        "    Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "N=201 Made=201 S=1605\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}
