// Codegen — issue #9060: a captured parameter declared by a bare type
// parameter `T`, at `T = string`, and WRITTEN inside a lambda body.
//
// Spec: §14.1 (v0.359) *Instantiation and its site* — "An instantiation of a
// generic declaration is the constructed type or constructed member obtained
// by substituting type arguments for its type parameters" (so in `G<string>`
// the parameter `p: T` is a `string` parameter); §27.5
// (v0.359) "Closures" — "Captured variables are heap-allocated in a closure
// object. Modifications to a captured variable inside the lambda are visible
// outside, and vice versa." (one heap cell, shared by the frame and every
// capturing env); §28.9 (v0.359) "Runtime-managed types" (`string` is
// refcounted: every co-owner takes exactly one reference and releases exactly
// one); §28.1 (v0.359) — `owned` is erased at a runtime-managed substitution of
// a bare type parameter, so the parameter is a BORROW of the caller's string.
//
// THE DEFECT. Every refcount decision about the cell is made on the
// SUBSTITUTED type — #2689's per-capture retain, the envdtor's content release
// and #9025's cell seed retain (fix #8946 classifies a captured `T` parameter
// at its concrete substitution) — but the WRITE `p := q` inside the lambda was
// a plain store: lowerStringLocalReassign asks the UNSUBSTITUTED expression
// type, which is `T`. So a write neither retained what it stored nor released
// what it displaced. MEASURED on r30-c1 @ 64d419f65 (`bezelc
// --sanitize=address`, 200 calls):
//
//   (A1) one env writes: the env's retain lands on the seed, its envdtor
//        releases the caller's `q`: `refcount over-release (string)`.
//   (A2) the writer runs, an escaping reader returns: the cell holds the
//        caller's `q` with no reference of its own, and the caller frees it
//        before the reader runs: heap-use-after-free.
//   (A3) the same with the writer nested in a second lambda: UAF.
//   (A4) one escaping env that writes and reads: over-release.
//   (A5) a loop of writes, escaping reader: UAF.
//
// THE FIX. The cell holder of such a parameter (the frame's post-boxing holder
// and every capturing lambda's binding of it, typeParamStringCellHolders_) is
// admitted by lowerStringLocalReassign, so the write retains the new value and
// releases the one it displaces, exactly as for a captured `string`
// parameter. Armed only when the cell owns its seed (the per-capture retain, or
// #9025's seed retain for a suppressed cell) and only when the FRAME body does
// not assign the parameter itself (the frame's own writes to a `T` parameter
// stay plain stores, the #4980 retain-policy question).
//
// ⚠ Every argument is built at runtime (an interpolation): a constant-folded
// literal is an RC-SENTINEL on which retain/release are no-ops.

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

// "q-0".."q-9" (3), "q-10".."q-99" (4), "q-100".."q-199" (5): a sum of
// |q-<j>| over the 200 calls is 30 + 360 + 500 = 890.

// =====================================================================
// (A1) ONE env writes the cell; 1 per call: 200.
// =====================================================================
const char* kSingleEnvWriter = R"BZ(
module P;
public class Program
  public static method G<T>(p: T, q: T): int
    var w := lambda() => do p := q; end;
    w();
    return 1;
  end G;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      t := t + Program.G<string>($"fr-{j}", $"q-{j}");
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
// (A2) the writer runs, the reader escapes: |q-<j>| per call, 890.
// =====================================================================
const char* kWriterRunsReaderEscapes = R"BZ(
module P;
public class Program
  public static method G<T>(p: T, q: T): () -> T
    var w := lambda() => do p := q; end;
    var r := lambda(): T => p;
    w();
    return r;
  end G;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.G<string>($"fr-{j}", $"q-{j}"); t := t + f().Length;
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
// (A3) the writer is nested in a second lambda: 890.
// =====================================================================
const char* kNestedInnerWriter = R"BZ(
module P;
public class Program
  public static method G<T>(p: T, q: T): () -> T
    var r := lambda(): T => p;
    var mk := lambda(): int => do
      var w := lambda() => do p := q; end;
      w();
      return 1;
    end;
    var n := mk();
    return r;
  end G;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.G<string>($"fr-{j}", $"q-{j}"); t := t + f().Length;
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
// (A4) one escaping env writes and reads, called twice: 1780.
// =====================================================================
const char* kSingleEnvEscapingWriter = R"BZ(
module P;
public class Program
  public static method G<T>(p: T, q: T): () -> T
    var w := lambda(): T => do p := q; return p; end;
    return w;
  end G;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.G<string>($"fr-{j}", $"q-{j}"); t := t + f().Length + f().Length;
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
// (A5) twenty writes per call, the reader escapes and reads |z-<j>|: 890.
// =====================================================================
const char* kLoopOfWritesEscaping = R"BZ(
module P;
public class Program
  public static method G<T>(p: T, q: T, z: T): () -> T
    var r := lambda(): T => p;
    var w := lambda() => do
      var i := 0;
      while i < 10 do
        p := q;
        p := z;
        i := i + 1;
      end;
    end;
    w();
    return r;
  end G;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.G<string>($"fr-{j}", $"q-{j}", $"z-{j}"); t := t + f().Length;
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

// (A1) Pre-fix: `refcount over-release (string)` abort, exit 134.
BEZEL_TEST(codegen_lambda_captured_type_param_cell_9060,
           single_env_write_retains_new_releases_old_spec_14_1) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kSingleEnvWriter), "200\n");
}

// (A2) Pre-fix: heap-use-after-free (no output).
BEZEL_TEST(codegen_lambda_captured_type_param_cell_9060,
           escaping_reader_after_write_reads_owned_value_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kWriterRunsReaderEscapes), "890\n");
}

// (A3) Pre-fix: heap-use-after-free.
BEZEL_TEST(codegen_lambda_captured_type_param_cell_9060,
           nested_inner_writer_cell_owns_value_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kNestedInnerWriter), "890\n");
}

// (A4) Pre-fix: `refcount over-release (string)` abort.
BEZEL_TEST(codegen_lambda_captured_type_param_cell_9060,
           single_escaping_env_write_retains_new_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kSingleEnvEscapingWriter), "1780\n");
}

// (A5) Pre-fix: heap-use-after-free. Post-fix: no memory error and a bounded
// leak however many writes: the final string and its buffer (the #2706 trade)
// plus the generic instantiation's 8-byte capture cell, which is not freed on
// the base either (MEASURED: 600 allocations; the never-written-before-escape
// p35 shape leaks the same 600 on r30-c1).
BEZEL_TEST(codegen_lambda_captured_type_param_cell_9060,
           loop_of_writes_releases_each_displaced_value_spec_28_9) {
    BEZEL_EXPECT_TRUE(leakedAllocationsOf(kLoopOfWritesEscaping, "890\n") <= 600ull);
}
