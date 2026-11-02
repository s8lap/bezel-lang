// Regression tests for #7906 -- an `owned` DECLARATION-LESS BUILT-IN CLASS
// used only as the receiver of a call (`Mk2().Next(1)` with
// `Mk2(): owned Random`) leaked the receiver: 100 allocations over 100
// iterations (MEASURED by the R37 rev-c2-45 review under generated-code
// LeakSanitizer: "SUMMARY: AddressSanitizer: 1600 byte(s) leaked in 100
// allocation(s)", every object from `bz_random_alloc`).
//
// It is the sibling of #7733, which did this for `StringBuilder`: #7733's
// chain-root claim (`markFreshStringBuilderChainRoot`) is STRINGBUILDER-TYPED
// -- its `isSbType` lambda tests `kind == Named && !t->declaration &&
// !t->nullable && builtinTypeName(t) == "StringBuilder"`, and the first THREE
// clauses are the DECLARATION-LESS-BUILTIN test, so only the name clause is
// StringBuilder-specific -- and its receiver site
// (`registerFreshBuiltinNewReceiverTempK8`) filters on the same name. A
// `Random` root was therefore never claimed, and a built-in instance call had
// no receiver site at all.
//
// Language spec §28.3 (v0.350) *Unbound owned temporaries* / K8.1, verbatim:
// "Unbound owned temporaries are disposed at their full expression's end, after
// their uses, in reverse production order -- on the normal edge as on the
// exception edge, where the pass runs during unwinding." §28.1: a method
// declared `owned T` transfers a fresh +1 to its caller.
//
// ── THE LEAK ORACLE (this is the point of the file) ────────────────────────
// A row that asserted only the program's OUTPUT would be GREEN on the pre-fix
// code and would prove nothing. Every row here measures a COUNT: the number of
// allocations LeakSanitizer reports as leaked, read out of LSan's own
// "SUMMARY: AddressSanitizer: <bytes> byte(s) leaked in <allocations>
// allocation(s)." line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO;
// on the pre-fix compiler the same rows read 100. The rows also run under
// `runAotChecked`, whose LSan leg turns a leak into a failed row, so the count
// and the verdict agree.
//
// Every row loops 100 times: a one-shot leak repro is a vacuous oracle.
//
// ── THE SOUNDNESS ROW ─────────────────────────────────────────────────────
// A method that merely returns its BORROWED parameter is not declared `owned`
// and must NOT be disposed: freeing a `Random` the caller still owns is a
// heap-use-after-free, which is worse than a leak. The `borrowed_...` row pins
// that the borrowed case is untouched: the receiver it returns is the SAME
// object the loop still owns, so disposing it would be a double free (ASan
// would report it) and NOT disposing it is what the row asserts.
//
// The `AtomicInteger<int>` row pins that the fix is not a one-name patch: a
// SECOND declaration-less built-in class (one whose release route is
// `bz_atomic_dispose`, not `bz_random_free`) is disposed on the same path.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated -- the sanitizer report
// the AOT harness embeds in the diagnostic message when LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// THE ORACLE. `leakedAllocations` is the COUNT of allocations LeakSanitizer
// reports as leaked: 0 when there is no SUMMARY line (LSan prints one only
// when it finds leaks), and the summary's own number when there is one. The
// pre-fix compiler produced 100 here; the fixed compiler produces 0.
//
// `leakedBytes` is the same measurement in bytes (1600 pre-fix: 100 objects of
// 16 B each from `bz_random_alloc`).
void expectNoLeak(const JitResult& r, const char* out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    // The COUNT, printed so a failing row's log carries the NUMBER the leak
    // oracle measured (the pre-fix compiler reads 100 allocations / 1600
    // bytes here, from `bz_random_alloc`).
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
}

}  // namespace

// THE ISSUE'S OWN REPRO, verbatim: `Mk2().Next(1)` with `Mk2(): owned Random`.
// 100 iterations; the receiver of every `Next` call is an unbound owned
// `Random` temporary that §28.3 K8.1 disposes at the full expression's end.
BEZEL_TEST(owned_builtin_chain_receiver_7906,
           owned_random_call_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Text; import System.Collections.Generic;
class C
  public static method Mk(): owned Random => new Random(42);
  public static method Mk2(): owned Random
    return new Random(42);
  end Mk2;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do t += Mk2().Next(1); end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// The `new`-rooted twin: the chain root is a `new Random(...)` rather than an
// `owned`-returning call. Both are FRESH PRODUCERS (§28.3's producer table:
// "Fresh productions are owned"); the `owned` marker is not what distinguishes
// them from a borrow.
BEZEL_TEST(owned_builtin_chain_receiver_7906,
           owned_random_new_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System;
class C
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do t += new Random(42).Next(1); end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// A SECOND declaration-less built-in class: `AtomicInteger<int>` (its release
// route is `bz_atomic_dispose`, NOT `bz_random_free`). The fix is not a
// one-name patch: the population is every declaration-less built-in class
// with a known release route.
BEZEL_TEST(owned_builtin_chain_receiver_7906,
           owned_atomic_integer_call_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System;
class C
  public static method Mk(): owned AtomicInteger<int> => new AtomicInteger<int>(0);
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do t += Mk().Increment(); end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}

// THE SOUNDNESS ROW (§28.1). `Mb` returns its parameter -- a BORROW, not an
// `owned` transfer -- so the `Random` it hands back is NOT the caller's to
// dispose, and the fix must leave it alone. `x` is owned by the loop body and
// is disposed at the loop body's end; if the fix disposed the borrowed
// receiver as well, `x` would be freed TWICE and ASan would report a
// double-free / heap-use-after-free. The row asserts the run is CLEAN (zero
// leaked allocations, zero sanitizer reports) -- a double free is a leak-oracle
// failure, so the row is RED if the borrow is ever disposed.
BEZEL_TEST(owned_builtin_chain_receiver_7906,
           borrowed_random_receiver_is_not_disposed_spec_28_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System;
class C
  public static method Mb(r: Random): Random => r;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      var x := new Random(42);
      t += Mb(x).Next(1);
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// K8.1's EXCEPTION EDGE: "on the normal edge as on the exception edge". The
// receiver (`Mk2()`) is produced FIRST (§22.5: the receiver precedes the
// argument list) and the ARGUMENT then throws, so the receiver is an
// already-produced, not-yet-consumed unbound owned temporary at the moment
// the unwinding starts -- §28.3's *Exception-path discharge* disposes it
// during unwinding. Pre-fix, all 100 receivers leaked on this edge too.
BEZEL_TEST(owned_builtin_chain_receiver_7906,
           owned_random_receiver_exception_edge_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System;
class C
  public static method Mk2(): owned Random
    return new Random(42);
  end Mk2;
  public static method Boom(): int
    throw new Exception("x");
  end Boom;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      try
        t += Mk2().Next(Boom());
      catch Exception => t += 1;
      end try;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}
