// Codegen — issue #9249: storing a BORROWED field's closure through a `ref`
// function-typed parameter (`x := src.F`) into a caller's CLASS FIELD
// (`Copy(ref a.F, b)`) OVER-RELEASED the closure: the field and the value's
// real owner both released the one reference.
//
// THE RULE.
//   §22.3 (v0.359), *`ref` local and field targets alias*:
//     "A `ref` argument naming a mutable local or writable field is a true
//     alias: callee writes are visible through the caller's location
//     immediately."
//   §28.3.1 (v0.359), rule 5 *Atoms*:
//     "Reads of storage lend: a field, property, indexer, or slot read is a
//     borrow whose source is the storage's holder."
//   §28.9 (v0.359), *Runtime-managed types*: function-typed values are
//     runtime-managed, so every reference to one is released exactly once.
//   So after `Copy(ref a.F, b)` the field `a.F` holds `b.F`'s closure exactly
//   as after the direct store `a.F := b.F`: the field needs its own
//   reference, and the closure it displaced is released once.
//
// THE DEFECT. The callee's store through the alias is a plain store: the
// callee cannot tell a class field (whose holder releases what it holds) from
// a local (released by nobody) or an accessor temporary (whose write-back
// retains), so it takes no reference — and a field holder then released a
// borrow it never owned.
//
// THE FIX (caller side; LlvmBackend_Assign.cpp, `recordClosureFieldRefArg`).
// The CALLER knows the slot is a class field. When the callee is a locally
// compiled, statically dispatched method whose body is only stores
// `x := p.G` from a class field of a by-value class-typed parameter (so it
// takes and gives no reference itself), the caller reads the field before the
// call and, right after it, retains the field's new value and releases the old
// one — the direct `a.F := b.F` store discipline. Locals and accessor targets
// are untouched (rows 4 and 5 pin that: the refuted callee-side retain,
// 891da6130, leaked both).
//
// THE ROWS (oracle: `runAotChecked`, generated-code ASan + LSan on).
//   1. the issue's literal repro, verbatim: RED on r30-c1 (over-release
//      abort, exit 134, no stdout); GREEN and leak-free with the fix.
//   2. the same mechanism over a slot that starts EMPTY (nullable field):
//      RED on r30-c1 (the same abort).
//   3. the +1-PRODUCER control (the callee stores a lambda): GREEN on both
//      arms; RED under a fix that also retains a producer (the extra
//      reference is never given back, so the cell leaks).
//   4. `Copy(ref xs[0], b)` with `xs: Array<() -> int>` (an INDEXER target,
//      copy-in/copy-out): GREEN on r30-c1, RED (LSan leak) on 891da6130.
//   5. `var v: () -> int; Copy(out v, b); v()` (an uninitialised LOCAL):
//      GREEN on r30-c1, RED (LSan leak) on 891da6130.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired; a marker otherwise. The exit code is asserted too, because a
// program that aborted on the over-release guard produces no stdout at all
// and would otherwise be indistinguishable from "printed the wrong value".
std::string runOut(const std::string& prog) {
    auto r = runAotChecked(prog);
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

// The +1-PRODUCER control: the callee stores a LAMBDA it mints, so the
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

// An INDEXER target (§22.3 "accessor targets bracket": copy-in / copy-out).
// The write-back stores the callee's value through the array's own setter,
// which takes the element's reference; the callee must take none.
const char* kArrayElemCopy = R"BZL(
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
      var b := new Hold(2);
      var xs: Array<() -> int> := [lambda() => 5, lambda() => 6];
      Copy(ref xs[0], b);
      sum := sum + xs[0]() + xs[1]();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZL";

// An uninitialised LOCAL passed `out`: the local is an alias of a slot that
// nothing releases, so the borrowed value stored there must not be retained.
const char* kOutLocalCopy = R"BZL(
module M;
import System;
public class Hold
  public F: () -> int;
  public constructor(n: int) F := lambda() => n; end constructor;
end Hold;
public class Program
  public static method Copy(out x: () -> int, src: Hold) x := src.F; end Copy;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var b := new Hold(2);
      var v: () -> int;
      Copy(out v, b);
      sum := sum + v();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZL";

}  // namespace

// Issue #9249's own program. On r30-c1 it aborts with "bezel: fatal:
// refcount over-release (closure) — two releases for one reference;
// terminating per §28.9" (exit 134) and prints nothing. With the fix it
// prints the sum and is leak-free (each displaced cell released once).
BEZEL_TEST(codegen_ref_param_closure_retain_9249,
           issue_repro_borrowed_field_stored_through_ref_param_spec_22_3_28_3_28_9) {
    BEZEL_EXPECT_EQ(runOut(kIssueRepro), "1600\n");
}

// The borrowed cell copied into the caller's slot stays callable after the
// source's holder is disposed — the field took its own reference — and
// the program is leak-free (the full ASan+LSan net).
BEZEL_TEST(codegen_ref_param_closure_retain_9249,
           borrowed_source_copy_keeps_the_slot_s_own_reference_spec_22_3_28_3_28_9) {
    BEZEL_EXPECT_EQ(runOut(kBorrowCopy), "19900\n");
}

// The +1-producer twin: the producer's single reference is transferred into
// the caller's slot, so nothing leaks and the value is the callee's.
BEZEL_TEST(codegen_ref_param_closure_retain_9249,
           producer_store_through_ref_param_transfers_its_reference_spec_22_3_28_3_28_9) {
    BEZEL_EXPECT_EQ(runOut(kProducerCopy), "1800\n");
}

// The indexer target keeps the copy-in / copy-out discipline: the array's
// write-back takes the element's reference, so the run is leak-free.
BEZEL_TEST(codegen_ref_param_closure_retain_9249,
           indexer_target_copy_back_takes_the_only_new_reference_spec_22_3_28_9) {
    BEZEL_EXPECT_EQ(runOut(kArrayElemCopy), "1600\n");
}

// An `out` local is a slot nothing releases: storing a borrow there takes no
// reference, so the run is leak-free.
BEZEL_TEST(codegen_ref_param_closure_retain_9249,
           out_local_borrow_store_takes_no_reference_spec_22_3_28_9) {
    BEZEL_EXPECT_EQ(runOut(kOutLocalCopy), "400\n");
}
