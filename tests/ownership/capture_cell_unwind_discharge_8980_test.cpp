// Regression test for issue #8980 — the capture cell of a PARAMETER captured
// by a NON-ESCAPING lambda leaks when an exception propagates out of the call
// that consumes the lambda (4 B per occurrence; 8 B for a `string` capture).
//
// THE DEFECT. A lambda that captures a local or a parameter of its enclosing
// body boxes the captured variable into a heap cell (§27.5) and the closure's
// env points at that cell. For a lambda that does NOT escape, the cell is
// reclaimed by a scope-end `boxedCellFree` disposal that the site OWNING the
// lambda registers. When the owning site is a CALL — a lambda passed to a
// call-bounded formal (§28.14) — that registration happens in the call site's
// NORMAL-return discharge (`releaseCallClosureArgs` ->
// `consumeLastLambdaCapturedCells`), which runs AFTER the call returns.
//
// So when the callee (or an operation inside it) THROWS, the registration never
// happens and the cell is never reclaimed: it leaks on every propagating call.
//
//   method M(p: int): int => Apply(lambda() => p + Boom(p));
//
// MEASURED on the base (r30-c1 @ 329a1d5ef), `bezelc --sanitize=address`:
//
//   Direct leak of 800 byte(s) in 200 object(s) allocated from:
//       #1 ... in bz$P.Program$M
//   SUMMARY: AddressSanitizer: 800 byte(s) leaked in 200 allocation(s).
//
// 4 B per call for the `int` capture (the cell holds the value) and 8 B for
// the `string` capture (the cell holds a pointer) — the leaked object is the
// CAPTURE CELL, not the string's heap content. The block-bodied and
// arrow-bodied method twins leak IDENTICALLY, and so does the twin that
// captures a LOCAL instead of a parameter.
//
// THE SPEC.
//   §27.5 (v0.359): "Captured variables are heap-allocated in a closure
//   object" — the cell is a heap allocation, so it is reclaimed with the
//   closure that owns it.
//   §28.9 (v0.359): the function value's storage is runtime-managed, so the
//   heap state the closure owns — the capture cell — is reclaimed with it.
//   §28.3 (v0.359) *Exception-path discharge*: "the compiler treats every
//   owned local and owned parameter as if it were wrapped in an implicit
//   try/finally … Exceptions therefore do not produce leaks." The cell is
//   owned state of the scope the capture lives in, so the exception edge must
//   discharge it too.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program), so the leak cannot hide under the sanitizer's noise.

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

// ROW 1 — the issue's literal repro, arrow-bodied method. The lambda captures
// the PARAMETER `p`; `Boom` throws from inside the lambda, so the exception
// propagates out of the consuming call `Apply(...)`.
//
// RED on the base: 800 B / 200 allocations (4 B per call — the `int` capture
// cell).
BEZEL_TEST(capture_cell_unwind_discharge_8980,
           arrow_bodied_method_throw_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Boom(p: int): int => throw new Exception(\"x\"); "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: int): int => Apply(lambda() => p + Boom(p)); "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        t := t + Program.M(j); "
        "      catch e: Exception => "
        "        t := t + 1; "
        "      end try; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// ROW 2 — the block-bodied method twin. Same defect, same leak: the shape of
// the METHOD body is not what decides it.
//
// RED on the base: 800 B / 200 allocations.
BEZEL_TEST(capture_cell_unwind_discharge_8980,
           block_bodied_method_throw_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Boom(p: int): int => throw new Exception(\"x\"); "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: int): int "
        "    return Apply(lambda() => p + Boom(p)); "
        "  end M; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        t := t + Program.M(j); "
        "      catch e: Exception => "
        "        t := t + 1; "
        "      end try; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// ROW 3 — the `string` capture: the cell holds a POINTER (8 B, not 4 B), and
// the closure's envdtor reads that pointer out of the cell to release the
// string it owns.
//
// This row is the ORDERING pin. The cell free must run AFTER the closure
// release that runs the envdtor; a fix that freed the cell in the scope's
// unwind disposal pass (which runs BEFORE the closure-argument cleanup block)
// would let the envdtor read a freed cell — a heap-use-after-free that ASan
// reports on this row.
//
// RED on the base: 1600 B / 200 allocations (8 B per call).
BEZEL_TEST(capture_cell_unwind_discharge_8980,
           string_capture_throw_reclaims_the_cell_and_its_content) {
    const std::string src =
        "module P; public class Program "
        "  public static method Boom(p: int): int => throw new Exception(\"x\"); "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: string): int "
        "    return Apply(lambda() => p.Length + Boom(p.Length)); "
        "  end M; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        t := t + Program.M(\"hi\"); "
        "      catch e: Exception => "
        "        t := t + 1; "
        "      end try; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// ROW 4 — the captured LOCAL twin. The cell is the same heap cell; only the
// captured symbol differs (a local, not a parameter), so the same reclaim
// applies (§27.5 / §28.9).
//
// RED on the base: 800 B / 200 allocations.
BEZEL_TEST(capture_cell_unwind_discharge_8980,
           captured_local_throw_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Boom(p: int): int => throw new Exception(\"x\"); "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: int): int "
        "    var q := p; "
        "    return Apply(lambda() => q + Boom(q)); "
        "  end M; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        t := t + Program.M(j); "
        "      catch e: Exception => "
        "        t := t + 1; "
        "      end try; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// ROW 5 — the block-bodied LAMBDA twin. The lambda's body form does not
// decide it either: the cell is reclaimed on every exit, whatever shape the
// lambda's body has.
//
// RED on the base: 800 B / 200 allocations.
BEZEL_TEST(capture_cell_unwind_discharge_8980,
           block_bodied_lambda_throw_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Boom(p: int): int => throw new Exception(\"x\"); "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: int): int "
        "    return Apply(lambda() => do "
        "      return p + Boom(p); "
        "    end); "
        "  end M; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        t := t + Program.M(j); "
        "      catch e: Exception => "
        "        t := t + 1; "
        "      end try; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// ROW 6 — the NORMAL path (the consuming call does NOT throw). This row is
// GREEN on the base: it is the suite's BOUNDARY CONTROL, kept because a
// corpus case cannot express it (the corpus runner has no ASan leg, so it can
// only pin compile + stdout, never "the cell is freed EXACTLY ONCE").
//
// The wrong fix it catches: freeing the cell on BOTH the unwind edge and the
// scope-end disposal — a double free, which ASan reports as
// "attempting double-free" and which fails this row.
//
// It also pins the normal path's stdout, so a fix that freed the cell at the
// wrong point (before the closure release) cannot pass by accident.
BEZEL_TEST(capture_cell_unwind_discharge_8980,
           normal_path_frees_the_capture_cell_exactly_once) {
    const std::string src =
        "module P; public class Program "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: int): int "
        "    return Apply(lambda() => p + 1); "
        "  end M; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      t := t + Program.M(j); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "20100\n");
}

}  // namespace
