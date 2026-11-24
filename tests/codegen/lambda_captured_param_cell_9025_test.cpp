// Codegen — issue #9025: the capture CELL of a WRITTEN, SHARED
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
// THE FIX (#9025). The cell mints one reference to its seed when it is
// ALLOCATED (boxLocalSlot's allocation branches, `cellSeedRetainSlots_`), so
// an escaping closure reads a value the cell keeps alive. The rows of group
// (A) run on `runAotCheckedLenient` (AddressSanitizer ON, LeakSanitizer OFF):
// the cell's final value is still leaked (the bounded #2706 / #2718 trade).
//
// ⚠ EVERY ARGUMENT IS BUILT AT RUNTIME (an interpolation): a constant-folded
// literal is an RC-SENTINEL on which retain/release are no-ops, and a row
// built on one cannot see the bug.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotCheckedLenient;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired (AddressSanitizer on, LeakSanitizer off); a marker otherwise.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
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
