// Regression test for issue #9115 — a RETURNED closure used directly as an
// index argument (`fl[g.MkClosure()]`) leaked its closure: the index
// argument's discharge recognised only the producers whose CELL is allocated
// in the caller's own frame (a lambda literal, a method reference, a
// function-typed property read), and DECLINED a call whose result is a +1
// closure.
//
// THE DEFECT. `Emitter::releaseIndexArgClosureProducers` — the caller's drop
// of an index argument's +1 closure producer — gated every release on
// `isPlusOneClosureProducer`, the predicate that admits a LambdaExpr literal
// or a bare/qualified method reference. It DELIBERATELY declines a
// closure-returning CALL, because that predicate cannot see the callee's body
// and so cannot know whether the callee minted a fresh +1 or handed back a
// borrow it still holds.
//
// The index argument is not that case. §23.5 (v0.359) forbids `ref`/`out`/
// `owned` on an indexer parameter list, so an indexer's function-typed
// parameter is plain and therefore CALL-BOUNDED (§28.14, v0.359: "the callee
// receives the callback for the duration of the call only"). The accessor
// never consumes the callback and keeps no reference to it, so the caller's
// +1 is the caller's to drop once the accessor has returned — whatever minted
// it. The two NARROW recognizers that DO know the callee mints exactly +1
// (`isUserMethodClosureReturnCall`: a call to a user method whose declared
// return type is a function type; `isLambdaClosureReturnCall`: a call through
// a lambda-typed local or parameter, whose body shape is statically known to
// mint +1) are the SAME pair the console discharge uses for an unbound closure
// temporary (LlvmBackend_Calls.cpp ~920) and the closure-local binding path
// uses to decide that the slot owns the producer's +1.
//
// MEASURED on r30-c1 @ 4e5dbd3fe, `bezelc --sanitize=address --link=system
// --checked-disposal`, the issue's own 200-iteration repro:
//
//   Direct leak of 12800 byte(s) in 200 object(s) allocated from:
//       #1 ... in bz$M.Gen$MkClosure ...
//   Indirect leak of 3200 byte(s) in 200 object(s) ...
//   Indirect leak of 1600 byte(s) in 200 object(s) ...
//   SUMMARY: AddressSanitizer: 17600 byte(s) leaked in 600 allocation(s).
//
// 17600 bytes over 600 allocations — the closure, its capture cell and the
// cell's payload, three allocations per call, all attributed to the caller's
// `MkClosure`. The program's own output is correct (16800).
//
// THE CHANGE. `releaseIndexArgClosureProducers`'s predicate is now the UNION
// of the closure producers an index argument can be:
//
//     if (!isPlusOneClosureProducer(ie) && !isUserMethodClosureReturnCall(ie) &&
//         !isLambdaClosureReturnCall(ie))
//         continue;
//
// The union is still NARROW: a CALLER-OWNED LOCAL passed as the index
// (`var g := lambda() => …; t[g];`) is matched by NONE of the three, so the
// local's own scope-end release keeps sole ownership of its +1.
//
// THE ORACLE. Every row runs the program TWICE: once plain (`runAot`) for the
// VALUE oracle — the program's own exit code and output — and once checked
// (`runAotChecked`) for the MEMORY oracle. An indexer GETTER copies no
// receiver (only the SETTER path does — a separate, pre-existing 16-byte
// receiver-copy defect this issue does not own), so these programs must leak
// NOTHING at all.
//
// ROWS.
//   Row 1 — the issue's own program, VERBATIM: `fl[g.MkClosure()]` and
//           `fl[g.F]` over 200 iterations. RED on the base (17600 B / 600
//           allocations), GREEN with the fix (no leak at all).
//   Row 2 — GREEN CONTROL: the plain getter with a LAMBDA LITERAL index
//           (`fl[lambda() => 7]`), the path #9083 / #9068 fixed. CLEAN on both
//           arms. It is the DOUBLE-RELEASE guard: a fix that released the
//           producer twice turns this row red with a bad free / use-after-free
//           under ASan.
//   Row 3 — GREEN CONTROL: the plain getter with a CALLER-OWNED closure LOCAL
//           as the index (`var g := lambda() => 7; fl[g]`), the path #9102
//           fixed. CLEAN on both arms. It is the OVER-BROAD guard: a predicate
//           that matched a local would release the local's +1 a second time,
//           and the local's own scope-end release would free it again.
//
// The issue's second shape — `fl[g.F]`, a read of a FUNCTION-TYPED property —
// is CLEAN on the base and with the fix (the property read is a
// `isPlusOneClosureProducer` producer, so the existing discharge already
// covered it). It is therefore NOT a gtest row (a row that is green on the
// base pins nothing); it is kept as a corpus candidate with its deciding
// spec sentence.
//
// per §28.14, §28.9, §23.4, §23.5 (issue #9115)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// The VALUE oracle: the program compiled, ran to completion, exited with its
// own exit code and printed exactly this output. The plain (uninstrumented)
// run has no sanitizer, so its exit code is the program's own.
void expectValue(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The MEMORY oracle: the checked run leaked NOTHING at all, and no memory
// error fired.
//
// An indexer GETTER copies no receiver (only the SETTER path does — a
// separate, pre-existing 16-byte receiver-copy defect this issue does not
// own), so a getter-only program has no residual: the closure's 17600 B / 600
// allocations over the issue's own 200-iteration repro is the WHOLE leak.
void expectNoLeak(const JitResult& r) {
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

// ── Row 1 (§28.14 / §28.9 / §23.5): the issue's own program, VERBATIM.
//
// `fl[g.MkClosure()]` — a call to a user method whose declared return type is
// a function type — is a +1 closure producer the CALLER owns; the indexer
// receives it as a call-bounded callback and keeps no reference to it, so the
// caller must drop it once the accessor returns.
//
// The arithmetic: `MkClosure` returns `lambda() => r.V` and `r.V` is 42, so
// each of the two reads adds 42; 200 iterations give 200 * (42 + 42) = 16800.
//
// RED on the base (17600 B / 600 allocations), GREEN with the fix.
BEZEL_TEST(indexer_returned_closure_arg_release_9115,
           returned_closure_index_argument_releases_its_closure) {
    const std::string src =
        "module M9115; "
        "import System; "
        "public class Res9115 "
        "  public V: int; "
        "  public constructor() V := 42; end constructor; "
        "end Res9115; "
        "public class Fl9115 "
        " public virtual property [q: () -> int]: int => q(); "
        " public constructor() end constructor; "
        "end Fl9115; "
        "public class Gen9115 "
        "  private owned _r: Res9115 := new Res9115(); "
        "  public method MkClosure(): () -> int "
        "    var r := _r; "
        "    return lambda() => r.V; "
        "  end MkClosure; "
        "  public property F: () -> int "
        "    get => do var r := _r; return lambda() => r.V; end; "
        "  end; "
        "  public constructor() end constructor; "
        "end Gen9115; "
        "public class Program9115 "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl := new Fl9115(); "
        "    var g := new Gen9115(); "
        "    for var j in 0..<200 do "
        "      t := t + fl[g.MkClosure()]; "
        "      t := t + fl[g.F]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program9115;";
    auto v = runAot(src);
    expectValue(v, 0, "16800\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 2 (GREEN CONTROL — the LAMBDA LITERAL index): the path #9083 / #9068
// fixed. CLEAN on the base and clean with the fix.
//
// It is the DOUBLE-RELEASE guard: a fix that released the producer twice (for
// example one that also released inside the accessor body) turns this row red
// with a bad free / use-after-free under ASan.
BEZEL_TEST(indexer_returned_closure_arg_release_9115,
           lambda_literal_index_argument_stays_clean) {
    const std::string src =
        "module L9115; "
        "import System; "
        "public class Fl9115L "
        " public virtual property [q: () -> int]: int => q(); "
        " public constructor() end constructor; "
        "end Fl9115L; "
        "public class Program9115L "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl := new Fl9115L(); "
        "    for var j in 0..<200 do "
        "      t := t + fl[lambda() => 7]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program9115L;";
    auto v = runAot(src);
    expectValue(v, 0, "1400\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 3 (GREEN CONTROL — the CALLER-OWNED closure LOCAL as the index): the
// path #9102 fixed. CLEAN on the base and clean with the fix.
//
// It is the OVER-BROAD guard: a predicate that matched a local would release
// the local's +1 a second time, and the local's own scope-end release would
// free it again (a double free under ASan).
BEZEL_TEST(indexer_returned_closure_arg_release_9115,
           caller_owned_closure_local_index_argument_stays_clean) {
    const std::string src =
        "module C9115; "
        "import System; "
        "public class Fl9115C "
        " public virtual property [q: () -> int]: int => q(); "
        " public constructor() end constructor; "
        "end Fl9115C; "
        "public class Program9115C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl := new Fl9115C(); "
        "    for var j in 0..<200 do "
        "      var g := lambda() => 7; "
        "      t := t + fl[g]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program9115C;";
    auto v = runAot(src);
    expectValue(v, 0, "1400\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

}  // namespace
