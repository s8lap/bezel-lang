// Regression test for issue #8984 — the capture cell of a PARAMETER captured
// by a NON-ESCAPING lambda in a NESTED scope of the same method is never
// reclaimed (4 B per occurrence; 8 B for a `string` capture), on EVERY exit.
//
// THE DEFECT. A lambda that captures a parameter of its enclosing body boxes
// the captured variable into a heap cell (§27.5) and the closure's env points
// at that cell. For a lambda that does NOT escape, the cell is reclaimed by a
// scope-end `boxedCellFree` disposal that the site OWNING the lambda
// registers — for a lambda handed to a call-bounded formal (§28.14), the
// call site's normal-return discharge (`releaseCallClosureArgs` ->
// `consumeLastLambdaCapturedCells`).
//
// The scope-end free is DECLINED outright when the captured symbol is a
// PARAMETER and the capture site is DEEPER than the scope the parameter is
// declared in (`consumeLastLambdaCapturedCells`:
//
//     if (ce.isParam &&
//         disposeStack_.size() > capturedParamDeclScopeDepthHere())
//         continue;
//
// ). That decline is #969's conservative skip, extended to parameters by
// #3403: the parameter may still be READ THROUGH THE CELL after the capture
// site's scope exits (freeing it there was a measured SIGSEGV). It is
// fail-closed — a bounded leak — but a parameter has no deferral route: the
// captured local's spelling defers the free to the local's DECLARING scope
// (`declScopeDepth_`), and that map holds LOCALS only. So the cell simply
// leaked.
//
//   method M(p: int): int
//     if p > 0 then
//       return Apply(lambda() => p + 1);   // the capture site is DEEPER
//     end if;                               // than `p`'s scope
//     return 0;
//   end M;
//
// MEASURED on the base (r43-f45-toolchain @ 0efd9b0ca), `bezelc
// --sanitize=address`, 200 calls (the first takes the else path, so 199
// captures):
//
//   20099
//   SUMMARY: AddressSanitizer: 796 byte(s) leaked in 199 allocation(s).
//
// The leaked object is the 4-byte boxed capture cell of `p` (§27.5),
// allocated in `bz$P.Program$M`. The TOP-LEVEL-scope twin of the same shape
// (`return Apply(lambda() => p + 1);` with no `if`) is CLEAN — the cell is
// freed there — which is the discriminator: the leak is the NESTED scope, not
// the capture.
//
// THE SPEC.
//   §27.5 (v0.359): "Captured variables are heap-allocated in a closure
//   object" — the cell is a heap allocation, so it is reclaimed with the
//   closure that owns it.
//   §28.9 (v0.359): runtime management governs "the function value itself —
//   the heap-allocated closure object that backs it" — the heap state the
//   closure owns (the capture cell) is reclaimed with it.
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

// ROW 1 — the issue's literal repro. The lambda captures the PARAMETER `p` in
// the `if` block's NESTED scope, which is deeper than the scope `p` is
// declared in (the method body's root scope). 199 of the 200 calls capture.
//
// RED on the base: 796 B / 199 allocations (4 B per call — the `int` capture
// cell).
BEZEL_TEST(capture_cell_nested_scope_8984,
           nested_if_scope_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: int): int "
        "    if p > 0 then "
        "      return Apply(lambda() => p + 1); "
        "    end if; "
        "    return 0; "
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
    expectClean(runAotChecked(src), 0, "20099\n");
}

// ROW 2 — the `do … end` nested-scope twin. The NESTED SCOPE is the only
// thing that decides the shape: a `do` block opens a scope exactly as the
// `if` block does, so the capture site is deeper than `p`'s scope here too.
//
// RED on the base: 800 B / 200 allocations.
BEZEL_TEST(capture_cell_nested_scope_8984,
           nested_do_block_scope_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: int): int "
        "    var s := 0; "
        "    do "
        "      s := Apply(lambda() => p + 1); "
        "    end; "
        "    return s; "
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

// ROW 3 — the #3403 HAZARD row, with the leak. The parameter is READ AFTER
// the nested scope has exited (`return s + p;`), which is exactly the read
// that #3403's SIGSEGV was about: after the capture, the parameter's storage
// is the cell, so a later read dereferences the cell.
//
// The fix must therefore defer the free to the scope `p` is DECLARED in (the
// method body's root scope) — the read above happens before that scope ends.
// A fix that frees the cell at the CAPTURE SITE's scope end (the shape
// #3403's conservative skip declined) turns this row into a SIGSEGV /
// use-after-free, so this row is red on the base (the leak) AND red under
// that plausible wrong fix.
//
// RED on the base: 796 B / 199 allocations.
BEZEL_TEST(capture_cell_nested_scope_8984,
           nested_scope_capture_with_the_parameter_read_after_the_block) {
    const std::string src =
        "module P; public class Program "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method M(p: int): int "
        "    var s := 0; "
        "    if p > 0 then "
        "      s := Apply(lambda() => p + 1); "
        "    end if; "
        "    return s + p; "
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
    expectClean(runAotChecked(src), 0, "39999\n");
}

// ROW 4 — the suite's BOUNDARY CONTROL: the TOP-LEVEL-scope twin of row 1
// (the capture site at the parameter's own depth). It is GREEN on the base —
// the cell is freed there — and it pins that the fix keeps freeing it
// EXACTLY ONCE. A corpus case cannot express this check (the corpus runner
// has no ASan leg, so it cannot count the frees); the plausible wrong fix it
// catches is a DOUBLE free — a fix that registers the deferred free AND keeps
// the scope-end free, or one that frees the cell on both the normal and the
// unwind edge.
BEZEL_TEST(capture_cell_nested_scope_8984,
           top_level_scope_capture_cell_is_freed_exactly_once) {
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
