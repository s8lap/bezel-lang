// Ownership — issue #9171: a DIRECT CALL of a function-typed `ref` PARAMETER
// (`x()` where `ref x: () -> int`) did not lower at all. The compiler printed
// only the #2491 `internal:` return-degrade note and the method silently
// returned 0, so the program printed a wrong value — a silent miscompile.
//
// Spec: §22.3 (v0.359) *Parameter modifiers* — the `ref` bullet: "Inside the
// callee the parameter is never flow-narrowed: its referent keeps the caller's
// other names for the whole call, so it is read at its declared type and
// narrowed through a local copy." §22.3 (v0.359) *`ref` and `out` argument
// locations* — "A `ref` argument naming a mutable local or writable field is a
// true alias: callee writes are visible through the caller's location
// immediately." §28.3.8 (v0.359) — "Where the location's declared type is a
// function type it is also a function-typed holder." §27.1 (v0.359) — a
// function-typed value is invoked through its closure pointer.
//
// THE DEFECT. MEASURED on r30-c1 @ 169718adf (`bezelc --sanitize=address`,
// 200 iterations — the issue's own program, verbatim):
//
//   p1_issue_repro.bzl:4:55: warning: internal: return-value expression failed
//       to lower to a value (degraded to a zero/null return value) — #2491
//   0                       ← expected 200
//
// A `ref` parameter's function value is the CONTENT of the caller's slot
// (§22.3: the parameter aliases a location the caller chose), so the call must
// read the closure pointer THROUGH the ref slot. The plain-function-typed-
// parameter twin (the issue's control) and the copy-through-a-local twin both
// print 200: the defect is in lowering a call whose callee is the `ref`
// parameter ITSELF. The `ref`-parameter-inside-a-lambda twin is CLEAN too (the
// capture registers the parameter's slot in `paramSlots_`, so the read already
// went through the shared cell) — both twins are corpus candidates, not rows.
//
// ⚠ THE `internal:` NOTE IS NOT ASSERTED HERE. The AOT gtest harness fills
// `JitResult::diagnostics` only on a compile REJECT, so an "assert no
// `internal:` note" written in gtest passes VACUOUSLY on any compiler, fixed
// or not — measured and recorded by #2963 (see
// tests/codegen/call_lowered_as_unit_vs_declined_2963_test.cpp). The
// diagnostic half is pinned in the lit track instead:
// tests/lit/ref_function_param_direct_call_no_degrade_note_9171.bzl.
//
// ⚠ NO ASAN LEG ON THE `out` ROW. That row's program STORES a closure into
// the recipient's slot, and the store leaks 128 B (the closure cell + its
// env). That leak is a PRE-EXISTING defect of the store, already filed as
// #9111, and it reproduces with NO call in the program at all (so it is not
// this issue's miscompile). An ASan leg there would pin #9111's leak rather
// than this fix.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired; a marker otherwise.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// =====================================================================
// (A1) The issue's verbatim repro: a method calls its own function-typed
// `ref` parameter directly. 200 iterations, each a fresh closure.
// Pre-fix: the #2491 `internal:` note, and the program prints 0.
// =====================================================================
const char* kIssueRepro = R"BZ(
module M;
import System;
public class Program
  public static method Peek(ref x: () -> int): int => x();
  public static method C(p: () -> int): int return Peek(ref p); end C;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      sum := sum + C(lambda() => 1);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A2) The block-bodied spelling of the same callee — the issue records
// that it behaves identically (the same note, then 0).
// =====================================================================
const char* kBlockBodied = R"BZ(
module M;
import System;
public class Program
  public static method Peek(ref x: () -> int): int return x(); end Peek;
  public static method C(p: () -> int): int return Peek(ref p); end C;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      sum := sum + C(lambda() => 1);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A3) The `out` sibling: the same lowering arm, the other by-ref
// modifier. §22.3: an `out` parameter "denotes a callee-local slot, freely
// readable after assignment", so invoking the function value it holds is an
// ordinary call. Pre-fix: the #2491 note, and the program prints 0.
// =====================================================================
const char* kOutThenCall = R"BZ(
module M;
import System;
public class Program
  public static method Fill(out x: () -> int): int
    x := lambda() => 5;
    return x();
  end Fill;
  public static method Main(): int
    var f: () -> int := lambda() => 0;
    Console.WriteLine(Fill(out f));
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A4) The generic sibling: `ref x: (T) -> T`. Pre-fix: the #2491 note
// (twice — once per instantiation), and the program prints 0.
// 200 iterations, each `lambda(a: int) => a + 1` applied to 1 ⇒ 2 each.
// =====================================================================
const char* kGenericRef = R"BZ(
module M;
import System;
public class Program
  public static method Apply<T>(ref x: (T) -> T, v: T): T
    return x(v);
  end Apply;
  public static method C<T>(p: (T) -> T, v: T): T return Apply(ref p, v); end C;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      sum := sum + C<int>(lambda(a: int) => a + 1, 1);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

}  // namespace

// (A1) Pre-fix: prints 0 (expected 200).
BEZEL_TEST(ownership_ref_function_param_direct_call_9171,
           issue_repro_arrow_body_spec_22_3) {
    auto r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
    // The ASan leg: the call really lowered, and nothing leaked.
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kIssueRepro), "200\n");
}

// (A2) Pre-fix: prints 0 (expected 200).
BEZEL_TEST(ownership_ref_function_param_direct_call_9171,
           issue_repro_block_body_spec_22_3) {
    auto r = runAot(kBlockBodied);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kBlockBodied), "200\n");
}

// (A3) Pre-fix: prints 0 (expected 5).
BEZEL_TEST(ownership_ref_function_param_direct_call_9171,
           out_param_then_call_spec_22_3) {
    auto r = runAot(kOutThenCall);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// (A4) Pre-fix: prints 0 (expected 400).
BEZEL_TEST(ownership_ref_function_param_direct_call_9171,
           generic_ref_param_call_spec_22_3) {
    auto r = runAot(kGenericRef);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kGenericRef), "400\n");
}
