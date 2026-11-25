// Codegen — issue #9249: storing a BORROWED field's closure through a `ref`
// function-typed parameter (`x := src.F`) OVER-RELEASED the closure. The store
// through the alias took no reference of its own to the value it stored, so
// the CALLER's slot and the value's REAL owner both released the same cell:
// the caller's release drove the count to zero and the real owner's release
// drove it negative.
//
// THE RULE.
//   §22.3 (v0.359), *`ref` local and field targets alias*:
//     "A `ref` argument naming a mutable local or writable field is a true
//     alias: callee writes are visible through the caller's location
//     immediately."
//     So the callee's `x := src.F` IS a store into the CALLER's slot — and the
//     caller's slot is a HOLDER of the function value it holds: a class field
//     releases the value it holds on the owner's disposal, a local releases
//     it at its scope end.
//   §28.3.1 (v0.359), rule 5 *Atoms*:
//     "Reads of storage lend: a field, property, indexer, or slot read is a
//     borrow whose source is the storage's holder."
//     So `src.F` is a BORROW — the source's holder keeps its own reference —
//     and the store that puts it into the caller's slot must take the slot's
//     OWN reference to it.
//   §28.9 (v0.359), *Runtime-managed types*:
//     "Function-typed values — values of any function type (T1, ..., Tn) -> R,
//     regardless of whether they capture" — the runtime manages their
//     lifecycle, so every reference to one is released exactly once.
//
// THE DEFECT. The assignment lowering had no closure counterpart to
// `lowerStringRefParamStore` (#2321): a `ref`/`out` parameter of type `string`
// gets load-old / retain-or-transfer-new / store-new / release-old, but a
// `ref`/`out` parameter of FUNCTION type fell through to a PLAIN store. The
// caller's slot therefore held a pointer it did not own, and the slot's own
// release (the field's disposal) dropped the SOURCE's reference.
//
// MEASURED on r30-c1 (d66898662), the issue's literal program verbatim
// (`scripts/agent_check.sh --probe --asan`):
//   "bezel: fatal: refcount over-release (closure) — two releases for one
//    reference; terminating per §28.9", exit 134, no stdout.
// With the fix the same program prints 1600 and exits 0.
//
// THE FIX (src/codegen/LlvmBackend_Stmts.cpp, the `:=` store dispatch). The
// store through a `ref`/`out` function-typed parameter takes the aliased slot's
// OWN reference to the value it stores: a +1 PRODUCER (a lambda, a method
// reference, a closure-returning call) is TRANSFERRED, and a BORROW (a field
// read, a local read, a parameter read) is RETAINED — the same
// retain-or-transfer decision `lowerClosureFieldStore` and
// `lowerClosureLocalReassign` make. A self-assignment (`x := x`) keeps the one
// reference the slot already holds, so the retain is emitted only when the
// slot's value CHANGES.
//
// The value the slot DISPLACES is released by the CALLER, which is the only
// party that can prove the slot owns its value — that is issue #9173's
// caller-side discharge (a callee-side release-old frees a cell the real
// owner still reaches, measured there as a heap-use-after-free). So row 1's
// program, whose slot IS displaced twice per iteration, still LEAKS the
// displaced cells: that residual leak is #9173's, and row 1 therefore runs
// under the LENIENT leak oracle (`runAotCheckedLenient` — generated-code
// AddressSanitizer on, LeakSanitizer off) for the same reason the #8723 rows
// do. Rows 2 and 3 are the leak-free shapes and run under the full
// `runAotChecked` net.
//
// THE ROWS.
//   1. the issue's literal repro, verbatim: 200 iterations, the sum printed.
//      RED on the base (the process aborts with the over-release guard, so
//      there is no stdout at all), GREEN with the fix.
//   2. the same mechanism in a shape that displaces NOTHING (the destination
//      slot starts empty), so the row can run under the FULL ASan+LSan net:
//      the borrowed cell is copied into the caller's slot and the source's
//      holder is then disposed — the copy must still be callable, and the
//      program must be leak-free. RED on the base (the same over-release
//      abort), GREEN with the fix.
//   3. the +1-PRODUCER twin of row 2 (the callee stores a lambda, not a
//      borrow): the producer's single reference is TRANSFERRED into the slot,
//      so the slot owns it and the program stays leak-free. GREEN on both
//      arms — the boundary control that pins the transfer/retain decision: it
//      goes RED under the plausible wrong fix that retains a producer as well
//      (the extra reference is never given back, so the cell leaks).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired; a marker otherwise. The exit code is asserted too, because a
// program that aborted on the over-release guard produces no stdout at all
// and would otherwise be indistinguishable from "printed the wrong value".
std::string runOut(const std::string& prog, bool lenient) {
    auto r = lenient ? runAotCheckedLenient(prog) : runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// The issue's repro, verbatim (the review lane's p23).
const char* kIssueRepro = R"BZL(
module M;
import System;
public class Hold
  public F: () -> int;
  public constructor(n: int) F := lambda() => n; end constructor;
end Hold;
public class Program
  public static method Copy(ref x: () -> int, src: Hold) x := src.F; end Copy;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var a := new Hold(1);
      var b := new Hold(2);
      var c := new Hold(3);
      Copy(ref a.F, b);
      Copy(ref a.F, c);
      sum := sum + a.F() + b.F() + c.F();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZL";

// The same mechanism over a slot that starts EMPTY, so nothing is displaced
// and the row can run under the full ASan+LSan net. `b.F` holds a closure
// that captures the loop variable; `Copy` stores that BORROW into the
// caller's slot `a.F`; the source holder `b` is disposed at the end of the
// iteration, so the copy must hold a reference of its own to stay callable.
const char* kBorrowCopy = R"BZL(
module M;
import System;
public class Hold
  public F: (() -> int)?;
  public constructor() end constructor;
end Hold;
public class Program
  public static method Copy(ref x: (() -> int)?, src: Hold) x := src.F; end Copy;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var a := new Hold();
      var b := new Hold();
      b.F := lambda() => i;
      Copy(ref a.F, b);
      sum := sum + a.F!();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZL";

// The +1-PRODUCER twin: the callee stores a LAMBDA it mints, so the
// producer's single reference is transferred into the caller's slot (no
// retain) and the slot owns it.
const char* kProducerCopy = R"BZL(
module M;
import System;
public class Hold
  public F: (() -> int)?;
  public constructor() end constructor;
end Hold;
public class Program
  public static method Reset(ref x: (() -> int)?) x := lambda() => 9; end Reset;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var a := new Hold();
      Reset(ref a.F);
      sum := sum + a.F!();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZL";

}  // namespace

// Issue #9249's own program. On the pre-fix tree it aborts with
// "bezel: fatal: refcount over-release (closure) — two releases for one
// reference; terminating per §28.9" (exit 134) and prints nothing.
BEZEL_TEST(codegen_ref_param_closure_retain_9249,
           issue_repro_borrowed_field_stored_through_ref_param_spec_22_3_28_3_28_9) {
    BEZEL_EXPECT_EQ(runOut(kIssueRepro, /*lenient=*/true), "1600\n");
}

// The borrowed cell copied into the caller's slot stays callable after the
// source's holder is disposed — the store took the slot's own reference — and
// the program is leak-free (the full ASan+LSan net).
BEZEL_TEST(codegen_ref_param_closure_retain_9249,
           borrowed_source_copy_keeps_the_slot_s_own_reference_spec_22_3_28_3_28_9) {
    BEZEL_EXPECT_EQ(runOut(kBorrowCopy, /*lenient=*/false), "19900\n");
}

// The +1-producer twin: the producer's single reference is transferred into
// the caller's slot, so nothing leaks and the value is the callee's.
BEZEL_TEST(codegen_ref_param_closure_retain_9249,
           producer_store_through_ref_param_transfers_its_reference_spec_22_3_28_3_28_9) {
    BEZEL_EXPECT_EQ(runOut(kProducerCopy, /*lenient=*/false), "1800\n");
}
