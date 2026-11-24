// Codegen — issue #9039: the capture CELL of a WRITTEN, SHARED captured
// FUNCTION-typed PARAMETER owned no reference to its seed (the closure twin of
// #9025 / #9026, tests/codegen/lambda_captured_param_cell_9025_test.cpp).
//
// Spec: §27.5 (v0.359) "Closures" — "Captured variables are heap-allocated in
// a closure object. Modifications to a captured variable inside the lambda are
// visible outside, and vice versa." (one heap cell, shared by the frame and
// every capturing env); §28.9 (v0.359) "Runtime-managed types" (a function
// value is runtime-managed: every co-owner takes exactly one reference and
// releases exactly one); §28.1 (v0.359) — "On any other type — a primitive,
// `string`, a function type, an ordinary record, an enum, a non-owning inline
// array, or the unit type — `owned` is a compile error" — so a by-value
// function-typed parameter is a BORROW whose single reference belongs to the
// CALLER.
//
// THE SHAPE. Two envs share one parameter's cell and one of them WRITES it:
//
//     public static method M(p: () -> string): string
//       var f1 := lambda(): string => p();
//       var f2 := lambda(): string => do ...; p := lambda(): string => s; ... end;
//
// #2706 (`mutatedSharedCaptureCell`) suppresses #2689's per-capture closure
// retain and the envdtor's content release for such a cell, so the cell's seed
// (the caller's borrowed closure) had no reference the cell owned. The cell's
// write path (lowerClosureLocalReassign / lowerCapturedClosureLocalReassign)
// releases the closure each write displaces, so the FIRST write dropped the
// caller's reference, and the caller's own release of its argument then
// aborted: `bezel: fatal: refcount over-release (closure) — two releases for
// one reference; terminating per §28.9`. MEASURED on r30-c1 @ 64d419f65
// (`bezelc --sanitize=address`) on every row of group (A) and on (B1).
//
// THE FIX. As for `string` (#9025), the cell mints a closure reference to its
// seed when it is ALLOCATED (boxLocalSlot's allocation branches,
// `cellSeedRetainSlots_` with CellSeedKind::Closure), unless the frame already
// holds a +1 of its own (retainAssignedClosureParams, #4174 —
// isOwnedClosureParamSlot). The cell then owns one reference to whatever it
// holds; its FINAL value still leaks (the bounded #2706 / #2718 trade).
//
// A function-typed parameter is call-bounded (§28.14), so no closure over it
// can escape the call: the hazard is the over-release alone, not a read after
// the caller's release.
//
// ⚠ Every argument closure captures a string built at runtime, so it is a heap
// closure with a real reference count.

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
// The pre-fix over-release abort is exit 134.
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

// The caller's closures return "b<j>" / "c<j>": "c0".."c9" (2), "c10".."c99"
// (3), "c100".."c199" (4), so a sum of |c<j>| over the 200 calls is
// 20 + 270 + 400 = 690 (and the same for b<j>).

// =====================================================================
// (A1) ISSUE #9039's REPRO, VERBATIM. f2 writes `p` twice; every read
// sees "c1": 6 per call, 1200.
// =====================================================================
const char* kIssueRepro9039 = R"BZ(
module P;
public class Program
  public static method M(p: () -> string): string
    var f1 := lambda(): string => p();
    var f2 := lambda(): string => do var s := $"c{1}"; p := lambda(): string => s; return p(); end;
    var x := f2();
    var y := f2();
    return x + y + f1();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}";
      var o := Program.M(lambda(): string => bb);
      t := t + o.Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A2) ASYNC frame: the writer runs on both sides of an `await` (the
// guarded boxing path an async body always takes). 1 + 1 + |c<j>|: 1090.
// =====================================================================
const char* kAsyncFrame = R"BZ(
module P;
public class Program
  public static async method M(p: () -> string, q: () -> string): Task<int>
    var r := lambda(): int => p().Length;
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    await Task.Delay(0);
    var b := w();
    return a + b + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}"; var cc := $"c{j}";
      using var k := Program.M(lambda(): string => bb, lambda(): string => cc);
      k.Wait();
      t := t + k.Result;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A3) ITERATOR frame. Yields 1, |c<j>|, 1: 1090.
// =====================================================================
const char* kIteratorFrame = R"BZ(
module P;
public class Program
  public static method M(p: () -> string, q: () -> string): IEnumerable<int>
    var r := lambda(): int => p().Length;
    var w := lambda(): int => do p := q; return 1; end;
    yield return w();
    yield return r();
    yield return w();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}"; var cc := $"c{j}";
      for var n in Program.M(lambda(): string => bb, lambda(): string => cc) do t := t + n; end;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A4) ONE lambda node inside a loop: three closures share one cell.
// The first pass reads |b<j>|, the next two |c<j>|: 690 + 2 * 690 = 2070.
// =====================================================================
const char* kCaptureInLoop = R"BZ(
module P;
public class Program
  public static method M(p: () -> string, q: () -> string): int
    var t := 0;
    for var i in 0..<3 do
      var w := lambda(): int => do p := q; return p().Length; end;
      t := t + w();
    end;
    return t;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}"; var cc := $"c{j}";
      t := t + Program.M(lambda(): string => bb, lambda(): string => cc);
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A5) The first capture site is CONDITIONAL, so a later capture
// allocates the cell from the pre-boxing slot (boxLocalSlot's #2303
// already-boxed branch). Per call: (j > 100 ? |b<j>| : 0) + 1 + |c<j>|;
// 99 * 4 + 200 + 690 = 1286.
// =====================================================================
const char* kConditionalFirstCapture = R"BZ(
module P;
public class Program
  public static method M(p: () -> string, q: () -> string, k: int): int
    var t := 0;
    if k > 100 then
      var r := lambda(): int => p().Length;
      t := t + r();
    end;
    var w := lambda(): int => do p := q; return 1; end;
    var r2 := lambda(): int => p().Length;
    t := t + w();
    return t + r2();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}"; var cc := $"c{j}";
      t := t + Program.M(lambda(): string => bb, lambda(): string => cc, j);
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A6) The write sits in a lambda NESTED inside a capturing lambda (the
// inner capture reuses the outer cell). 1 + |c<j>| + |c<j>|: 1580.
// =====================================================================
const char* kNestedInnerWriter = R"BZ(
module P;
public class Program
  public static method M(p: () -> string, q: () -> string): int
    var r := lambda(): int => p().Length;
    var mk := lambda(): int => do
      var w := lambda(): int => do p := q; return 1; end;
      return w() + p().Length;
    end;
    return mk() + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}"; var cc := $"c{j}";
      t := t + Program.M(lambda(): string => bb, lambda(): string => cc);
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (B1) TEN writes of a fresh closure per call, in a `while` loop. The
// cell owns one reference to every value it holds, so each write releases
// the closure it displaces and only the FINAL one leaks: one closure
// (one allocation) per call. 1 + |"kk"| per call: 600.
// =====================================================================
const char* kLoopOfFreshWrites = R"BZ(
module P;
public class Program
  public static method M(p: () -> string): int
    var r := lambda(): int => p().Length;
    var w := lambda(): int => do
      var i := 0;
      while i < 10 do
        p := lambda(): string => "kk";
        i := i + 1;
      end;
      return 1;
    end;
    var a := w();
    return a + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}";
      t := t + Program.M(lambda(): string => bb);
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

}  // namespace

// (A1) Pre-fix: `refcount over-release (closure)` abort, exit 134. Post-fix:
// 1200.
BEZEL_TEST(codegen_lambda_captured_fn_param_cell_9039,
           issue_repro_shared_written_fn_param_cell_owns_seed_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kIssueRepro9039), "1200\n");
}

// (A2) Pre-fix: over-release abort. Post-fix: 1090.
BEZEL_TEST(codegen_lambda_captured_fn_param_cell_9039,
           async_frame_shared_written_fn_param_cell_owns_seed_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kAsyncFrame), "1090\n");
}

// (A3) Pre-fix: over-release abort. Post-fix: 1090.
BEZEL_TEST(codegen_lambda_captured_fn_param_cell_9039,
           iterator_frame_shared_written_fn_param_cell_owns_seed_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kIteratorFrame), "1090\n");
}

// (A4) Pre-fix: over-release abort. Post-fix: 2070.
BEZEL_TEST(codegen_lambda_captured_fn_param_cell_9039,
           capture_in_loop_written_fn_param_cell_owns_seed_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kCaptureInLoop), "2070\n");
}

// (A5) Pre-fix: over-release abort. Post-fix: 1286.
BEZEL_TEST(codegen_lambda_captured_fn_param_cell_9039,
           conditional_first_capture_fn_param_cell_owns_seed_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kConditionalFirstCapture), "1286\n");
}

// (A6) Pre-fix: over-release abort. Post-fix: 1580.
BEZEL_TEST(codegen_lambda_captured_fn_param_cell_9039,
           nested_inner_writer_fn_param_cell_owns_seed_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kNestedInnerWriter), "1580\n");
}

// (B1) Pre-fix: over-release abort (no output). Post-fix: at most ONE leaked
// closure per call, however many writes (MEASURED: 200 allocations).
BEZEL_TEST(codegen_lambda_captured_fn_param_cell_9039,
           loop_of_fresh_writes_releases_each_displaced_closure_spec_28_9) {
    BEZEL_EXPECT_TRUE(leakedAllocationsOf(kLoopOfFreshWrites, "600\n") <= 200ull);
}
