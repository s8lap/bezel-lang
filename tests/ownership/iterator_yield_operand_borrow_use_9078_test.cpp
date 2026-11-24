// Regression test for issue #9078 — the §28.3.9 consume-site freeze's use
// pre-scan had NO arm for the `yield return` statement, so a borrow holder
// that is read ONLY inside a `yield return` operand was invisible to the
// freeze's liveness query and the consume of its source was admitted.
//
// THE DEFECT (MEASURED on the base r30-c1 @ cbd5a44a3, `bezelc`):
//
//   module M;
//   public class Res
//     public N: int := 7;
//     public constructor() end constructor;
//   end Res;
//   public class Sink
//     public static method Take(owned r: Res): int => 1;
//   end Sink;
//   public class Program
//     public static method Gen(): IEnumerable<int>
//       var a := new Res();
//       var f := lambda(): int => a.N;
//       var t := Sink.Take(move a);
//       yield return t;
//       yield return f();
//     end Gen;
//     public static method Main(): int
//       var r := 0;
//       for var x in Gen() do r := r + x; end;
//       Console.WriteLine($"{r}");
//       return 0;
//     end Main;
//   end Program;
//
// The program COMPILES on the base and the run dies with an ASan SEGV in the
// closure — measured:
//
//   ==3222826==ERROR: AddressSanitizer: SEGV on unknown address 0x00000000000c
//       #0 lam$0
//       #1 bz$iter$M.Program$Gen$MoveNext
//   SUMMARY: AddressSanitizer: SEGV bezel in lam$0
//
// (probes/9078_repro.bzl; `scripts/agent_check.sh --probe --asan`).
//
// THE SPEC.
//
//   §28.3.9 (v0.359) *The consume-site freeze*: "A borrow is *live* at a
//   consume site when some path from the site reaches a later use of it —
//   K5.5's body-local backward dataflow, traced directly, through a `using
//   var` pin (§28.10), or through a closure value that is itself still live."
//
// The closure `f` holds a borrow of `a`, and `f` is read — USED — at the
// `yield return f();` that FOLLOWS the `move a`. That read is a later use of
// the borrow on the path from the consume site, so the borrow is live at the
// `move a` and the consume must be rejected. The freeze's body-local pre-scan
// (`MethodOwnership::collectBorrowUsesStmt`, src/ownership/Ownership_Checks.cpp)
// records a read for every statement form it knows — the assignment, the
// declaration, the expression, the `return`, the `throw`, the discard — and
// had no arm for the yield statement, so this one read went unrecorded and
// the consume was admitted. The yield operand is an ordinary operand read,
// exactly like the `return` operand the same walk already records.
//
// THE ADMITTING TWIN (the shape the fix must NOT reject) is the mirror image:
// the closure's LAST use precedes the consume, so the borrow is dead at the
// consume site and the transfer is legal (§28.3.9 asks about a LATER USE OF
// THIS BORROW). It is green on the base and green on the fix; it is kept as a
// CORPUS CANDIDATE (not a gtest row) because its program leaks the closure's
// capture cell under LeakSanitizer — a pre-existing, unrelated defect filed
// separately — so it cannot be a gtest row under the forced-ASan sweep.
//
// ORACLE. `runAot` — the strict AOT path (fork-isolated `Driver::compileFile`
// + clang link + native exec). Both rows assert the SPECIFIC diagnostic the
// §28.3.9 freeze reports, so a row is RED on the base (where the program
// compiles and the run SEGVs — the compile-time diagnostic is absent) and
// GREEN with the fix (where the program is rejected at the `move`).
// Row 1's program repeats the iterator 200 times, so the base arm's
// use-after-free is a real, repeated runtime defect rather than a one-shot
// accident — under `BEZEL_AOT_FORCE_SANITIZE=address` the same row's ASan
// leg reports it.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// The shared prelude: the owned local the closure borrows, and the owned sink
// that consumes it.
constexpr const char* kPrelude =
    "module M; "
    "public class Res "
    "  public N: int := 7; "
    "  public constructor() end constructor; "
    "end Res; "
    "public class Sink "
    "  public static method Take(owned r: Res): int => 1; "
    "end Sink; ";

// The freeze's diagnostic, as §28.3.9 states it: the consume site names the
// consumed binding, the borrow holder, and the borrow's surviving use.
void expectConsumeSiteFreeze(const JitResult& r) {
    // The program must not run: it is rejected at the `move a`.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot consume `a` here"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "borrow held by `f`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "consume-site freeze"));
}

}  // namespace

// ROW 1 — the issue's literal repro. The closure `f` (a borrow of the owned
// local `a`) is read only inside a `yield return` operand, and that read
// FOLLOWS the `move a` — so the borrow is live at the consume site and the
// consume must be rejected.
//
// RED on the base: the program compiles clean there (the freeze's pre-scan
// never saw the `yield return f();` operand) and the run SEGVs in `lam$0`,
// so no §28.3.9 diagnostic is reported.
BEZEL_TEST(iterator_yield_operand_borrow_use_9078,
           yield_operand_use_after_the_move_is_rejected) {
    const std::string src =
        std::string(kPrelude) +
        "public class Program "
        "  public static method Gen(): IEnumerable<int> "
        "    var a := new Res(); "
        "    var f := lambda(): int => a.N; "
        "    var t := Sink.Take(move a); "
        "    yield return t; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var r := 0; "
        "    for var j in 0..<200 do "
        "      for var x in Gen() do r := r + x; end; "
        "    end; "
        "    Console.WriteLine($\"{r}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectConsumeSiteFreeze(runAot(src));
}

// ROW 2 — the loop-free twin of row 1: the same shape with NO loop anywhere
// (neither in the iterator body nor in `Main`). It pins that the freeze sees
// the yield operand as a use on its own account, and not through any loop
// bookkeeping.
//
// RED on the base for the same reason as row 1: the program compiles clean
// and the closure is called on freed memory.
BEZEL_TEST(iterator_yield_operand_borrow_use_9078,
           loop_free_yield_operand_use_after_the_move_is_rejected) {
    const std::string src =
        std::string(kPrelude) +
        "public class Program "
        "  public static method Gen(): IEnumerable<int> "
        "    var a := new Res(); "
        "    var f := lambda(): int => a.N; "
        "    var t := Sink.Take(move a); "
        "    yield return t; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"1\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectConsumeSiteFreeze(runAot(src));
}
