// Codegen — an EXPRESSION-bodied lambda owes the §28.3.11 discharge its body
// produces, and had no scope to register it into (§28.3.11 *Unbound owned
// temporaries*, §28.11 *Element slot policy*, v0.359).
//
// THE DEFECT. §28.3.11 disposes an unbound owned temporary "at the end of the
// full expression in which it is produced, after every use of it within that
// expression has completed". A lambda's expression body IS a full expression
// (K8), so `P.MakeDict()["aa"].Length` inside `lambda() => P.MakeDict()["aa"]
// .Length` must discharge the `owned Dictionary` the call produced.
//
// `Emitter::lowerLambda` (src/codegen/LlvmBackend_Exprs2.cpp) clears
// `disposeStack_` before the body and then lowers the EXPRESSION body with no
// scope pushed at all. The BLOCK-bodied form goes through `lowerScopedBlock`,
// which pushes a scope and calls `emitDisposals` at scope end — that is why the
// block-bodied twin of this very repro is clean. With `disposeStack_` empty,
// the #6476 registration in `lowerCollectionIndexerGet`
// (src/codegen/LlvmBackend_Assign.cpp) — the indexer-get path's §28.3.11
// discharge point — declined (`disposeStack_.empty()`), and the whole table
// leaked.
//
// MEASURED on this lane's base (r30-c1 @ f2589f706), 101 iterations,
// `bezelc --sanitize=address`, `ASAN_OPTIONS=detect_leaks=1`:
//
//   var f := lambda() => P.MakeDict()["aa"].Length;
//     stdout 303, exit 1
//     SUMMARY: AddressSanitizer: 49995 byte(s) leaked in 606 allocation(s)
//
// 606 = 6 x 101: per iteration the `bz_dict_alloc` header, its bucket array,
// the `bz_dict_set_str` key byte-copy, and three string allocations. The leak
// is the WHOLE TABLE. The BLOCK-bodied twin of the same repro measures 0
// bytes / 0 allocations on the same base, and the figure here matches the
// verification lane's (r42-v-c4) measurement of the same tip.
//
// WHY THE DISCHARGE MUST RUN AFTER THE RETAIN. The value the indexer-get
// hands back is read OUT of the receiver table, and a `string`-returning body
// retains it (`emitStringRetain`) before returning it. A disposal that ran
// first would free the very pointer the retain reads. The second row below is
// that shape: it is the load-bearing soundness row — a leak fixed by
// over-releasing, or a discharge that ran before the retain, shows up there
// and nowhere else.
//
// A LEAK PIN MUST PROVE ITS DETECTOR RAN. `runAot` defaults to
// `ASAN_OPTIONS=detect_leaks=0`, so a leak row on THAT harness is VACUOUS —
// green on a broken tree while showing a clean sanitizer trio. Every leak row
// below therefore runs on `runAotChecked`, which arms LeakSanitizer BY
// CONSTRUCTION (tests/support/aot_support.hpp). The knob
// `BEZEL_AOT_DETECT_LEAKS` is NOT the arming mechanism and is deliberately NOT
// set here.
//
// EVERY LEAK ROW IS PAIRED WITH A CRASH ORACLE. `leakedBytes` structurally
// CANNOT see a use-after-free: the abort suppresses LSan's summary, so a UAF
// reads as 0 leaked — the vacuous-oracle trap in its purest form. Each pair
// runs the same program under `runAotCheckedLenient` and asserts it ran to
// completion, printed the expected bytes, and exited 0. A leak fixed by
// OVER-releasing shows up here and nowhere else; over-releasing is a REJECT,
// not a trade.
//
// LEAK BOUNDS ARE AN EXACT ZERO, NOT A CEILING. A ceiling (`leaked <= 2728`)
// passes at 0 as readily as at 7,272, so it cannot distinguish "discharged"
// from "declined". Every `leakedBytes` row below asserts `== 0`.
//
// EVERY ROW BELOW IS RED ON THE BASE. There is no base-green gtest row in this
// file: the block-bodied twin and the bound-receiver twin are green on both
// arms by design, and they are left in the lane's probe directory under
// `corpus-candidates/` for the harvest lane, each with the spec sentence that
// decides its verdict.

#include <initializer_list>
#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The harness SKIPS (loudly, counted) when clang is absent; a row that asserts
// through that would be asserting on an unarmed oracle. A sanitizer finding is
// NOT a skip — "sanitizer fired" means it ran and found something.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// 101 iterations — a one-shot leak repro is a vacuity trap: one evaluation's
// table is small enough to hide under LSan noise, and the defect repeats once
// per evaluation. Every number quoted in this file is a 101-iteration
// measurement.
const char* kIterations = "i < 101";

// THE ISSUE'S REPRO'S PRODUCER, byte for byte. The value is a CONCATENATION
// producer (`"va" + "a"`), so it allocates its own +1 rather than aliasing an
// immortal literal — a leak on this shape is therefore several allocations per
// iteration, which is the 606 the base measured, and it is large enough to
// clear LSan noise rather than hide under it.
std::string strValProducer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, string> "
        "    var d: Dictionary<string, string> := new Dictionary<string, string>(); "
        "    d.Add(\"aa\", \"va\" + \"a\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; ";
}

// Bytes LSan reports for `program`, or 0 when it reports none. The trap this
// shape hides is why every caller pairs it with a crash row — see the header.
long leakedBytes(const std::string& program) {
    // `runAotChecked` arms LSan by construction (see the header). No env knob.
    JitResult r = runAotChecked(program);
    static const std::regex kLeak(R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak)) return std::stol(m[1]);
    }
    return 0;
}

// The CRASH oracle: run to completion, correct stdout, exit 0. `r.ran` is the
// load-bearing assertion — false iff the generated program died on a memory
// error. A row whose printed CONTENT is more than one value uses the
// `expectRunsCleanAll` form below: EVERY expected value must appear in stdout,
// so a row cannot pass by merely avoiding the fault.
void expectRunsCleanAll(const std::string& program,
                        std::initializer_list<const char*> expected) {
    JitResult r = runAotCheckedLenient(program);
    // Loud-first, silent-second, in that order.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    for (const char* e : expected)
        BEZEL_EXPECT_TRUE(r.stdoutText.find(e) != std::string::npos);
}

void expectRunsClean(const std::string& program, const char* expected) {
    expectRunsCleanAll(program, {expected});
}

// A LEAK-oracle row: LSan armed by `runAotChecked` itself, program runs clean.
void expectNoLeak(const std::string& program, const char* expected) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expected) != std::string::npos);
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
    }
}
// ---------------------------------------------------------------------------

// THE ISSUE'S OWN REPRO, ASSERTED AT ZERO. `n` accumulates `.Length` so the
// value the indexer-get reads is observably USED — a dead store could be
// optimized into a shape that never materializes the reference, which would
// make this row vacuously green.
//
// MEASURED on the base (r30-c1 @ f2589f706): exit 1, stdout 303,
// 49995 byte(s) leaked in 606 allocation(s).
BEZEL_TEST(expr_body_lambda_scope_discharge_6480,
           the_issue_repro_releases_the_indexer_get_receiver_spec_28_3) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    var f := lambda() => P.MakeDict()[\"aa\"].Length; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + f(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "303");
}

// THE STRING-VALUED TWIN — the load-bearing soundness row. Here the body
// returns the VALUE the indexer-get read out of the receiver, so the lambda
// retains it (`emitStringRetain`) before returning it to its caller. The
// receiver's discharge must run AFTER that retain: a discharge that ran first
// would free the very pointer the retain reads (a use-after-free), and one
// that ran without the retain would hand the caller a pointer to freed memory.
//
// MEASURED on the base (r30-c1 @ f2589f706): exit 1, stdout 303,
// 49995 byte(s) leaked in 606 allocation(s).
BEZEL_TEST(expr_body_lambda_scope_discharge_6480,
           a_string_valued_expr_body_releases_its_receiver_spec_28_3) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    var f := lambda() => P.MakeDict()[\"aa\"]; "
        "    while " + std::string(kIterations) + " do "
        "      var s := f(); "
        "      n := n + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "303");
}

// THE INT-VALUED TWIN. A `Dictionary<string,int>` has no co-owned +1 on its
// value column at all, so there is nothing for the free to release there — this
// row says the discharge is installed by column category and not sprayed. It
// is also a leak oracle in its own right: the base leaked the table here too.
//
// MEASURED on the base (r30-c1 @ f2589f706): exit 1, stdout 707,
// 46359 byte(s) leaked in 404 allocation(s).
BEZEL_TEST(expr_body_lambda_scope_discharge_6480,
           an_int_valued_expr_body_releases_its_receiver_spec_28_3) {
    std::string program =
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, int> "
        "    var d: Dictionary<string, int> := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 7); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    var f := lambda() => P.MakeDict()[\"aa\"]; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + f(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "707");
}

// THE CLASS-VALUED TWIN. §28.11's element-category table gives a class slot
// its own row ("borrow out" on indexer-get), so this is the shape where the
// free's value-column disposer must NOT be aimed at a co-owned +1 that does
// not exist. The Box is a `Main` local, not a fresh `new Box()` argument: an
// unbound owned temporary passed to a BORROWING `Dictionary.Add` is itself an
// undischarged §28.3 temporary (issue #6481), a SEPARATE pre-existing defect.
// Binding it here keeps this row's leak count attributable to the receiver
// alone.
//
// MEASURED on the base (r30-c1 @ f2589f706): exit 1, stdout 4242,
// 43935 byte(s) leaked in 505 allocation(s).
BEZEL_TEST(expr_body_lambda_scope_discharge_6480,
           a_class_valued_expr_body_releases_its_receiver_spec_28_3) {
    std::string program =
        "module M; "
        "class Box public N: int := 42; end Box; "
        "class P public "
        "  static method MakeDict(b: Box): owned Dictionary<string, Box> "
        "    var d: Dictionary<string, Box> := new Dictionary<string, Box>(); "
        "    d.Add(\"aa\", b); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var b := new Box(); "
        "    var i := 0; "
        "    var n := 0; "
        "    var f := lambda() => P.MakeDict(b)[\"aa\"].N; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + f(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "4242");
}

}  // namespace
