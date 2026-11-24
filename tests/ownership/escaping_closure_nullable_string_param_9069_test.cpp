// Regression test for issue #9069 — an ESCAPING closure over a by-value
// `string?` PARAMETER read the CALLER's freed fresh-temporary argument (a
// heap-use-after-free), while the non-nullable `string` twin was clean.
//
// THE DEFECT. A closure that captures a variable of its enclosing body boxes
// that variable into a heap capture cell (§27.5) and the closure's env points
// at the cell. For a captured PARAMETER the cell's content is the CALLER's
// value: §28.1 (v0.359) makes `owned` a compile error on a `string`, so the
// parameter is an unmodified BORROW and the caller keeps the reference — it
// releases its argument when the call returns.
//
// The reference-counting that keeps a captured value alive was keyed on the
// NON-NULLABLE `string` alone (`isStringTypeRefNonNull`): the capture site's
// per-capture retain, the envdtor's matching release, the #2706 "written
// shared cell" suppression and the capture cell's own seed reference all
// declined the nullable form. So a captured `string?` parameter's cell held a
// PURE BORROW of the caller's value: an escaping closure read it after the
// caller had released it.
//
//   AddressSanitizer: heap-use-after-free
//     READ of size 8 ... in lam$0
//     freed by thread T0 here: ... in bz$P.Program$Run
//
// MEASURED on the base (r30-c1 @ 3e1197d83), `bezelc --sanitize=address`, the
// issue's own repro: `ERROR: AddressSanitizer: heap-use-after-free`, no
// output. The non-nullable twin (`p: string`, `p.Length`) printed 1090,
// clean.
//
// THE SPEC.
//   §27.5 (v0.359): "Captured variables are heap-allocated in a closure
//   object" — the capture is a heap cell the closure owns, and the closure
//   may outlive the frame that formed it.
//   §28.9 (v0.359): `string` is runtime-managed — "the type's lifetime is
//   managed by the runtime without user involvement". A value the runtime
//   manages cannot be freed while a holder still reads it.
//   §28.1 (v0.359): `owned` is a compile error on a `string`, so a by-value
//   `string`/`string?` parameter is an unmodified borrow of the caller's
//   reference — which is exactly why the CAPTURE must take a reference of its
//   own (the twin's behaviour since #2689).
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program), so the defect cannot hide under the sanitizer's noise.
//
// ⚠ WHY THE STRINGS ARE BUILT AT RUNTIME. A constant-folded literal is an
// RC-SENTINEL on which the refcount calls are no-ops, so a literal-built probe
// passes whether or not the defect is present. Every program below builds its
// strings with interpolation (`$"fr-{j}"`), which allocates.
//
// ⚠ THE VALUES. "fr-0".."fr-199" are 4, 5 and 6 characters long: 10*4 + 90*5
// + 100*6 = 1090. "q-0".."q-199" are 3, 4 and 5 characters long: 10*3 + 90*4
// + 100*5 = 890.

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

// ROW 1 — the issue's literal repro. The closure over the by-value `string?`
// PARAMETER escapes the call; the caller's argument is a fresh temporary the
// caller releases when the call returns.
//
// RED on the base: `heap-use-after-free`, READ of size 8 in `lam$0`, freed in
// `bz$P.Program$Run` — the caller's release of the temporary.
BEZEL_TEST(ownership_escaping_closure_nullable_string_param_9069,
           escaping_closure_over_a_nullable_string_parameter_keeps_the_value_alive) {
    const std::string src =
        "module P; public class Program "
        "  public static method M(p: string?): () -> int "
        "    return lambda(): int => (p ?? \"\").Length; "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.M($\"fr-{j}\"); "
        "      t := t + f(); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1090\n");
}

// ROW 2 — TWO closures over the same `string?` parameter. The second capture
// reuses the first capture's cell, so the cell's content is shared and each
// closure's envdtor drops the reference the capture took. The #2706
// suppression ("a written, shared cell suppresses BOTH the per-capture retain
// and the envdtor release") must reach the nullable form too, or the retain
// and the release name different strings.
//
// RED on the base: `heap-use-after-free` in `lam$1`.
BEZEL_TEST(ownership_escaping_closure_nullable_string_param_9069,
           two_closures_over_the_same_nullable_string_parameter_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method M(p: string?, q: string?): () -> int "
        "    var w := lambda(): int => do return 1; end; "
        "    var r := lambda(): int => (p ?? \"\").Length; "
        "    var a := w(); "
        "    return r; "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.M($\"fr-{j}\", $\"q-{j}\"); "
        "      t := t + f(); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1090\n");
}

// ROW 3 — the parameter is WRITTEN inside the closure. The write displaces the
// cell's content, so the drop-on-overwrite release must be licensed by a
// reference the CELL owns (the reference the capture took, or the cell's own
// seed reference). The nullable form's write path took the plain-store route
// instead, so the cell's content became a bare borrow of the caller's value —
// and the escaping closure read it after the caller released it.
//
// The write puts the SECOND argument into the cell, so the value read back is
// "q-0".."q-199" (890), not "fr-0".."fr-199".
//
// RED on the base: `heap-use-after-free` in the escaping closure.
BEZEL_TEST(ownership_escaping_closure_nullable_string_param_9069,
           a_nullable_string_parameter_written_inside_the_escaping_closure) {
    const std::string src =
        "module P; public class Program "
        "  public static method M(p: string?, q: string?): () -> int "
        "    return lambda(): int => do p := q; return (p ?? \"\").Length; end; "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.M($\"fr-{j}\", $\"q-{j}\"); "
        "      t := t + f(); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "890\n");
}

// ROW 4 — the suite's BOUNDARY CONTROL: the NON-NULLABLE twin of row 1, which
// the issue itself reports as already correct. It is green on the base and
// pins that the fix did not change the twin's refcount balance — a plausible
// wrong fix (a retain taken twice, or the envdtor's release widened to the
// captured-LOCAL path) makes this row LEAK +1 per capture, which LSan sees.
//
// A corpus case cannot express this check (the corpus runner has no ASan leg,
// so it cannot count the references); the plausible wrong fix it catches is
// named in the commit body.
BEZEL_TEST(ownership_escaping_closure_nullable_string_param_9069,
           the_non_nullable_string_parameter_twin_stays_balanced) {
    const std::string src =
        "module P; public class Program "
        "  public static method M(p: string): () -> int "
        "    return lambda(): int => p.Length; "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.M($\"fr-{j}\"); "
        "      t := t + f(); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1090\n");
}

}  // namespace
