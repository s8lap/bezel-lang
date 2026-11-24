// Codegen — issues #9025 and #9026: the capture CELL of a WRITTEN, SHARED
// captured `string` PARAMETER owned no reference to its seed.
//
// Spec: §27.5 (v0.359) "Closures" — "Captured variables are heap-allocated in
// a closure object. Modifications to a captured variable inside the lambda are
// visible outside, and vice versa." (one heap cell, shared by the frame and
// every capturing env); §28.9 (v0.359) "Runtime-managed types" (`string` is
// refcounted: every co-owner takes exactly one reference and releases exactly
// one); §28.1 (v0.359) "Where the `owned` modifier can appear" — "On any other
// type — a primitive, `string`, a function type, an ordinary record, an enum,
// a non-owning inline array, or the unit type — `owned` is a compile error:
// such values carry no disposal obligation to transfer (§28.9)" — so a
// by-value `string` parameter is a BORROW whose single reference belongs to
// the CALLER.
//
// THE SHAPE. Two envs share one parameter's cell and one of them WRITES it:
//
//     public static method Ident(p: string): () -> string
//       var f1 := lambda(): string => do return p; end;
//       var f2 := lambda(): string => do p := p + "z"; return p; end;
//       return f2;
//     end Ident;
//
// #2706 (`mutatedSharedCaptureCell`) suppresses BOTH #2689's per-capture
// retain and the envdtor's content release for such a cell, so the cell's seed
// — the caller's borrowed argument — had no reference the cell owned.
//
//   #9025 [UAF]. An ESCAPING closure reads the cell after `Ident` returns. With
//   a fresh-temporary argument (`Ident($"fr-{j}")`) the caller frees it as soon
//   as the call returns, and the closure reads freed memory. MEASURED on
//   r30-c1 @ e598654aa, `bezelc --sanitize=address`: `heap-use-after-free`
//   (READ of size 8) on every row of group (A).
//
//   #9026 [leak]. #9005's remedy for the same missing reference skipped the
//   write's drop-on-overwrite release for EVERY write through such a cell, so
//   every displaced value leaked — including the cell's own +1s after the
//   first write — and the leak grew with the number of writes. MEASURED on
//   r30-c1 @ e598654aa (LeakSanitizer, 200 calls): 8000 allocations for 20
//   writes per call (B1), 1200 for the frame-owned shape (B2), 800 for two
//   `+=` writes (B3).
//
// THE FIX. The cell mints one reference to its seed when it is ALLOCATED
// (boxLocalSlot's allocation branches, `cellSeedRetainSlots_`), and the
// write's release is no longer skipped. The cell then owns exactly one
// reference to whatever it holds. Its FINAL value still leaks — the envdtor
// release stays suppressed, because no one env of a shared cell is its last
// holder (the bounded #2706 / #2718 trade) — so the leak is ONE string (two
// allocations: the string and its buffer) per call, whatever the number of
// writes.
//
// ⚠ EVERY ARGUMENT IS BUILT AT RUNTIME (an interpolation): a constant-folded
// literal is an RC-SENTINEL on which retain/release are no-ops, and a row
// built on one cannot see the bug.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired (AddressSanitizer on, LeakSanitizer off); a marker otherwise.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// Run under AddressSanitizer + LeakSanitizer and return the number of leaked
// ALLOCATIONS LeakSanitizer counted (0 when it reported none). The run must
// have printed `expectedOut`, and NO memory error other than a leak may have
// fired: no AddressSanitizer report, no §28.9 over-release abort.
unsigned long long leakedAllocationsOf(const std::string& prog,
                                       const std::string& expectedOut) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_EQ(r.stdoutText, expectedOut);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("did not compile") == std::string::npos);
    unsigned long long bytes = 0, allocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, bytes, allocs);
    // A leak report whose SUMMARY could not be read is not "no leak".
    if (!sawSummary &&
        text.find("ERROR: LeakSanitizer") != std::string::npos)
        return ~0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << allocs << " allocation(s) / "
                  << bytes << " byte(s)\n";
    return sawSummary ? allocs : 0ull;
}

// A fully clean run: expected output, exit 0, no sanitizer report at all.
void expectClean(const std::string& prog, const std::string& expectedOut) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedOut);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

// The argument lengths: "fr-0".."fr-9" (4), "fr-10".."fr-99" (5),
// "fr-100".."fr-199" (6) — so a sum of L over the 200 calls is
// 40 + 450 + 600 = 1090.

// =====================================================================
// (A1) ISSUE #9025's REPRO, VERBATIM. f2 escapes and writes the cell
// after Ident returned: L + 1 per call, 1090 + 200 = 1290.
// =====================================================================
const char* kIssueRepro9025 =
    "module P; "
    "public class Program "
    "  public static method Ident(p: string): () -> string "
    "    var f1 := lambda(): string => do return p; end; "
    "    var f2 := lambda(): string => do p := p + \"z\"; return p; end; "
    "    return f2; "
    "  end Ident; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var f := Program.Ident($\"fr-{j}\"); "
    "      t := t + f().Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (A2) THE ISSUE's NESTED FORM: the outer lambda escapes, and the inner
// one reaches the same cell through the §27.5 nesting. Two inner calls per
// Ident call share the cell: (L + 1) + (L + 2), 2180 + 600 = 2780.
// =====================================================================
const char* kNestedFresh =
    "module P; "
    "public class Program "
    "  public static method Ident(p: string): () -> () -> string "
    "    return lambda() => lambda(): string => do p := p + \"z\"; return p; end; "
    "  end Ident; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var f := Program.Ident($\"fr-{j}\"); "
    "      t := t + f()().Length + f()().Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (A3) ONE lambda NODE inside a LOOP: every pass's closure shares the one
// guarded cell (#2706's loop arm), and the last one escapes. (L + 1) +
// (L + 2) per call: 2780.
// =====================================================================
const char* kCaptureInLoopEscaping =
    "module P; "
    "public class Program "
    "  public static method M(p: string): () -> string "
    "    var g := lambda(): string => p; "
    "    for var i in 0..<3 do "
    "      g := lambda(): string => do p := p + \"q\"; return p; end; "
    "    end; "
    "    return g; "
    "  end M; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var g := Program.M($\"fr-{j}\"); "
    "      t := t + g().Length + g().Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (A4) THE FIRST CAPTURE SITE IS CONDITIONAL. On the calls where `c` is
// false the cell is allocated by the SECOND capture site (boxLocalSlot's
// already-boxed allocation branch, #2303), which must mint the seed's
// reference too. (L + 1) + (L + 2) per call: 2780.
// =====================================================================
const char* kConditionalFirstCapture =
    "module P; "
    "public class Program "
    "  public static method M(p: string, c: bool): () -> string "
    "    var f1 := lambda(): string => \"x\"; "
    "    if c then "
    "      f1 := lambda(): string => p; "
    "    end; "
    "    var f2 := lambda(): string => do p := p + \"z\"; return p; end; "
    "    var x := f1(); "
    "    return f2; "
    "  end M; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var g := Program.M($\"fr-{j}\", j mod 2 = 0); "
    "      t := t + g().Length + g().Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (B1) ISSUE #9026's FIRST PROBE: a literal argument, a lambda that
// assigns the captured parameter 20 times per call, shared with a reader.
// "lit" + 20 × "z" (23) + the reader's "lit…" (23) = 46 per call: 9200.
// Pre-fix every displaced value leaked: 8000 allocations over 200 calls.
// =====================================================================
const char* kLoopOfWrites =
    "module P; "
    "public class Program "
    "  public static method M(p: string): string "
    "    var f1 := lambda(): string => p; "
    "    var f2 := lambda(): string => do "
    "      for var i in 0..<20 do p := p + \"z\"; end; "
    "      return p; "
    "    end; "
    "    var r := f2(); "
    "    return f1() + r; "
    "  end M; "
    "  public static method Run(): int "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var o := Program.M(\"lit\"); "
    "      t := t + o.Length; "
    "    end; "
    "    return t; "
    "  end Run; "
    "  public static method Main(): int "
    "    Console.WriteLine(Program.Run()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (B2) THE FRAME ALSO ASSIGNS `p` (#2696's bind-time +1 is the frame's,
// and the cell inherits it): the lambda-side holder must keep its release
// too. s = "hi-1" (4): p = "hi-1a" (5), f2 -> "hi-1azy" (7), the frame's
// own write -> "hi-1azyb" (8), f1 reads it (8): 7 + 8 + 8 + 4 = 27 per
// call, 5400. Pre-fix: 1200 allocations over 200 calls.
// =====================================================================
const char* kFrameAlsoAssigns =
    "module P; "
    "public class Program "
    "  public static method M(p: string): string "
    "    p := p + \"a\"; "
    "    var f1 := lambda(): string => p; "
    "    var f2 := lambda(): string => do p := p + \"z\"; p := p + \"y\"; return p; end; "
    "    var r := f2(); "
    "    p := p + \"b\"; "
    "    return f1() + r + p; "
    "  end M; "
    "  public static method Run(): int "
    "    var s := $\"hi-{1}\"; "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var o := Program.M(s); "
    "      t := t + o.Length + s.Length; "
    "    end; "
    "    return t; "
    "  end Run; "
    "  public static method Main(): int "
    "    Console.WriteLine(Program.Run()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (B3) THE §4.6 `+=` WRITE PATH: two compound writes. s = "hi-1" (4):
// f2 -> "hi-1zy" (6), f1 reads it (6), + s (4) = 16 per call, 3200.
// Pre-fix: 800 allocations over 200 calls.
// =====================================================================
const char* kCompoundWrites =
    "module P; "
    "public class Program "
    "  public static method M(p: string): string "
    "    var f1 := lambda(): string => p; "
    "    var f2 := lambda(): string => do p += \"z\"; p += \"y\"; return p; end; "
    "    var r := f2(); "
    "    return f1() + r; "
    "  end M; "
    "  public static method Run(): int "
    "    var s := $\"hi-{1}\"; "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var o := Program.M(s); "
    "      t := t + o.Length + s.Length; "
    "    end; "
    "    return t; "
    "  end Run; "
    "  public static method Main(): int "
    "    Console.WriteLine(Program.Run()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (C1) CONTROL — a captured parameter that is RETURNED BY NAME and never
// WRITTEN (#4198 suppresses its per-capture retain, but nothing releases
// a displaced value): the borrow is balanced as is, and a seed reference
// would only leak. L per call: 1090, leak-free on the base and with the
// fix. RED under the plausible wrong fix that mints the seed for every
// suppressed capture (measured: 400 allocations).
// =====================================================================
const char* kReturnedNeverWritten =
    "module P; "
    "public class Program "
    "  public static method M(p: string): string "
    "    var f := lambda(): string => p + \"r\"; "
    "    var x := f(); "
    "    return p; "
    "  end M; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var o := Program.M($\"fr-{j}\"); "
    "      t := t + o.Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (C2) CONTROL — ONE env, no loop, written and escaping: #2689's
// per-capture retain is TAKEN and the envdtor releases the final value,
// so the cell is balanced. (L + 1) + (L + 2) per call: 2780, leak-free on
// the base and with the fix. RED under the plausible wrong fix that mints
// the seed for every written captured parameter (measured: 400
// allocations).
// =====================================================================
const char* kSingleEnvWrittenEscaping =
    "module P; "
    "public class Program "
    "  public static method Ident(p: string): () -> string "
    "    return lambda(): string => do p := p + \"z\"; return p; end; "
    "  end Ident; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var f := Program.Ident($\"fr-{j}\"); "
    "      t := t + f().Length + f().Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

// (A1) Pre-fix: heap-use-after-free READ in Main (no output). Post-fix: 1290.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           issue_repro_escaping_writer_reads_fresh_temporary_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kIssueRepro9025), "1290\n");
}

// (A2) Pre-fix: heap-use-after-free. Post-fix: 2780.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           nested_escaping_writer_reads_fresh_temporary_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kNestedFresh), "2780\n");
}

// (A3) Pre-fix: heap-use-after-free. Post-fix: 2780.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           loop_capture_escaping_writer_reads_fresh_temporary_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kCaptureInLoopEscaping), "2780\n");
}

// (A4) Pre-fix: heap-use-after-free. Post-fix: 2780.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           conditional_first_capture_cell_owns_seed_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kConditionalFirstCapture), "2780\n");
}

// (B1) At most ONE leaked string (2 allocations) per call; pre-fix 8000.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           loop_of_writes_releases_each_displaced_value_spec_28_9) {
    BEZEL_EXPECT_TRUE(leakedAllocationsOf(kLoopOfWrites, "9200\n") <= 400ull);
}

// (B2) At most ONE leaked string per call; pre-fix 1200.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           frame_owned_param_lambda_write_releases_displaced_spec_28_9) {
    BEZEL_EXPECT_TRUE(leakedAllocationsOf(kFrameAlsoAssigns, "5400\n") <= 400ull);
}

// (B3) At most ONE leaked string per call; pre-fix 800.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           compound_writes_release_each_displaced_value_spec_4_6) {
    BEZEL_EXPECT_TRUE(leakedAllocationsOf(kCompoundWrites, "3200\n") <= 400ull);
}

// (C1) Control: fully clean on both arms.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           control_returned_never_written_param_takes_no_seed_ref_spec_28_9) {
    expectClean(kReturnedNeverWritten, "1090\n");
}

// (C2) Control: fully clean on both arms.
BEZEL_TEST(codegen_lambda_captured_param_cell_9025,
           control_single_env_written_param_takes_no_seed_ref_spec_27_5) {
    expectClean(kSingleEnvWrittenEscaping, "2780\n");
}
