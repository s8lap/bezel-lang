// Regression test for issue #9292 — a BARE method group naming an INSTANCE
// method of the enclosing class (`xs.Select(Conv)`) passed to a LAZY stdlib §5
// LINQ operator leaked the closure cell it formed, 72 B per call (64 B cell +
// 8 B receiver capture); the qualified spelling `xs.Select(this.Conv)` was clean.
//
// THE RULE. §27.6 (v0.359) forms a method reference as a function value, and
// §28.9 (v0.359) puts a function value's backing closure object under the
// runtime's lifetime: each formation mints a fresh +1 cell that its holder
// must release at its own release point. stdlib §5 (v0.359): a callback passed
// to a LAZY operator is "retained by the returned query value until
// enumeration", so the ENUMERATOR is the holder and releases the cell at its
// dispose (`bz_lazy_own_closure` / `bz_lazy_dispose`).
//
// THE DEFECT. #9216 made the shared `isPlusOneClosureProducer` admit a bare
// identifier naming an instance method (`makeMethodThunk` ->
// `buildMethodClosureCell` mints a cell for it exactly as for `this.Conv`).
// The LAZY lowering's own producer test (`argIsFreshClosureCell`,
// src/codegen/LlvmBackend_Calls3.cpp, #7160) still carried a hand-copied mirror
// of the OLD static-only predicate, so `ownClosureIf` never armed
// `bz_lazy_own_closure` for the bare instance group and the cell leaked.
//
// THE FIX. `argIsFreshClosureCell` asks the SHARED predicate (restricted to a
// method symbol) instead of mirroring it.
//
// ORACLE. `runAotChecked` (ASan over the generated module + LeakSanitizer),
// each row loops 200x so the leak cannot hide under sanitizer noise, and each
// asserts the program's own output so a silent zero-fill cannot pass.

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

}  // namespace

// ROW 1 — the issue's own repro: `xs.Select(Conv)` with a bare instance group,
// 200 calls. Each call sums (2+3+4) = 9, so 1800.
//
// RED on the base: 12800 B direct + 1600 B indirect leaked in 200 allocations.
BEZEL_TEST(lazy_bare_instance_method_group_closure_release_9292,
           select_bare_instance_group_cell_is_released) {
    const std::string src =
        "module M; "
        "public class P "
        "  public method Conv(x: int): int => x + 1; "
        "  public method Make(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var t := 0; "
        "    for var y in xs.Select(Conv) do t := t + y; end; "
        "    return t; "
        "  end Make; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0 ..< 200 do "
        "      var p := new P(); "
        "      total := total + p.Make(); "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end P;";
    expectClean(runAotChecked(src), 0, "1800\n");
}

// ROW 2 — a second lazy operator (`Where`) with a bare instance PREDICATE group,
// so the fix is not specific to `Select`'s selector lowering. Each call keeps
// the elements with Keep(x) true (2 and 3) and sums them: 5; 200 calls = 1000.
//
// RED on the base: one 72 B cell leaked per call.
BEZEL_TEST(lazy_bare_instance_method_group_closure_release_9292,
           where_bare_instance_group_cell_is_released) {
    const std::string src =
        "module M; "
        "public class P "
        "  public method Keep(x: int): bool => x > 1; "
        "  public method Make(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var t := 0; "
        "    for var y in xs.Where(Keep) do t := t + y; end; "
        "    return t; "
        "  end Make; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0 ..< 200 do "
        "      var p := new P(); "
        "      total := total + p.Make(); "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end P;";
    expectClean(runAotChecked(src), 0, "1000\n");
}

// CONTROL — the bare instance group stored / returned / passed through a
// local, beside the now-owned LINQ argument: the stored and returned spellings
// already own their cell through the local / return protocols, and the LINQ
// arm must not release anything else (a wrong fix that treated every bare
// instance identifier as the enumerator's would double-free the stored one;
// ASan reports it). 200 iterations x (Conv via local 2 + Select sum 9) = 2200.
BEZEL_TEST(lazy_bare_instance_method_group_closure_release_9292,
           stored_and_returned_bare_instance_group_not_over_released) {
    const std::string src =
        "module M; "
        "public class P "
        "  public method Conv(x: int): int => x + 1; "
        "  public method Get(): (int) -> int "
        "    return Conv; "
        "  end Get; "
        "  public method Make(): int "
        "    var f := Conv; "
        "    var g := Get(); "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var t := f(0) + g(0); "
        "    for var y in xs.Select(Conv) do t := t + y; end; "
        "    return t; "
        "  end Make; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0 ..< 200 do "
        "      var p := new P(); "
        "      total := total + p.Make(); "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end P;";
    expectClean(runAotChecked(src), 0, "2200\n");
}
