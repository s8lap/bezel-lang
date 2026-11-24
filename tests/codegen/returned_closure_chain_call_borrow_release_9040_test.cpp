// Codegen / memory-safety — issue #9040 (spec §28.3.5 *The announcement
// table* row 4, v0.359; §28.3 *Borrow lifetime rules*; §28.3.11):
// a closure returned from a method over a BORROWED class parameter, invoked
// as a TEMPORARY (`Program.Use(s2)()`), freed the parameter's object while
// the result still referenced it — an ASan heap-use-after-free in `main`.
//
// THE PROGRAM (the issue's own repro, reduced to its two shapes):
//
//   public static method Use(p: K): () -> K
//     var f := lambda() => p;      // the closure borrows the PLAIN
//     var g := lambda() => f();    // parameter `p` (row 4's source set)
//     return lambda() => g();
//   end Use;
//   ...
//   var s2 := new K(5);
//   var r2 := Program.Use(s2)();   // the chain-call (temporary) form
//   Console.WriteLine(r2.N);
//
// THE DEFECT. `Use`'s closure captures the plain (borrowing) parameter `p`,
// so the invocation's result is a BORROW of the caller's `s2` — §28.3.5's
// announcement table, row 4, *Function-value invocation* column: "Borrow,
// sources = the function value's source set … **The result is never
// owned**". The ownership pass agrees: `producerState`'s function-value arm
// classifies a plain class/interface slot's invocation result as BORROWED,
// so the local the result is bound to carries no disposal obligation.
//
// The CODEGEN contradicted it. The CG60 chain-call arm (LlvmBackend_Calls2.cpp)
// registered a scope-end disposal for the chain-call result for EVERY
// class/record-typed return slot — owned or not — so a BORROWED result was
// DISPOSED at the end of the full expression, freeing an object the caller
// does not own. The disposal freed `s2`'s object; `s2`'s own scope-end
// disposal then read the freed object:
//
//   ERROR: AddressSanitizer: heap-use-after-free
//   READ of size 8 … in main            (s2's scope-end disposal)
//   freed by thread T0 here: free …     (the chain-call result's disposal)
//
// MEASURED on r30-c1 @ 5697baf74 (`bezelc --sanitize=address`): the repro
// above aborts with that report; the BOUND twin (`var h := Program.Use(s);
// h()`) is clean, because its callee is an identifier — the result is bound
// straight into the local's slot and no chain-call disposal was ever
// registered for it.
//
// THE FIX (src/codegen/LlvmBackend_Calls2.cpp). The scope-end disposal is
// registered only when the announcement table says the caller OWNS the
// chain-call result: row 1 (`owned T`), rows 2/3 (a mint-announcing declared
// slot — the Task family and the eight iterable-family heads — calls mint),
// and the owning-wrapper/owning-collection heads. Every other slot is row
// 4's borrow — the result is never owned — and a borrowed value is not
// disposed: §28.3.11 disposes an unbound temporary at the end of the full
// expression, but only an OWNED one ("Such a temporary" = an owned value).
//
// The SEMA COMPANION (src/sema/ExprTypecheck_Exprs.cpp). The #1820
// callee-side check for row 4's "Lambda into such a slot: … a fresh owned
// value rejected" admitted a lambda's body only when the lambda had NO
// return-type ascription; the ascription was a way past the check. The
// ascription does not change the slot's kind (the CALLER reads the TARGET's
// function type, and the ascription only constrains the body's types
// against it), so the check now fires for an ascribed lambda too. That
// closes the shape issue #9040's family is about: a lambda minting a fresh
// frame into a plain class/interface slot — a program no caller would ever
// dispose — is REJECTED rather than accepted-and-leaked. The legal spelling
// is `() -> owned T` on the TARGET (a lambda ascription cannot carry an
// `owned` marker at all: the grammar's `owned` appears only in a function
// type's return slot, a parameter modifier, and an inline-array element).
//
// The rows below. Four RED rows are RED on r30-c1 for the reason named in
// their own comment (the heap-use-after-free, and the callee-side rule that
// the ascription used to bypass), and GREEN after the fix. TWO BOUNDARY
// CONTROLS are GREEN on r30-c1 as well; each names the plausible wrong fix it
// is red under, per `docs/ops/lane-contract.md` point 7 ("at most two gtest
// boundary controls per suite … each red under a plausible wrong fix that the
// commit body names").
//
// A third green-on-both-arms program — the BOUND twin of the issue's repro —
// is NOT a gtest row (no plausible wrong fix of this commit turns it red):
// it is filed as a corpus candidate under
// `/var/tmp/bezel-scratch/r43-m30-9040/probes/corpus-candidates/`.
//
// THE HARNESS. One of the rows runs under `runAotCheckedLenient` (the
// generated-code AddressSanitizer on, LeakSanitizer off) because its program
// hits a PRE-EXISTING, separate defect: the escaping closure's capture cell
// leaks (8 B per `Use` call — 1600 B over the 200 iterations below), on
// r30-c1 before this fix and in the bound form too. That leak is not this
// issue's subject and is filed separately (#9081); the memory-error net this
// issue needs stays on in every row. The other rows run under the full
// `runAotChecked` net (they are leak-clean).

#include <string>

#include "support/aot_support.hpp"        // runAotChecked — ASan/LSan AOT harness
#include "support/aot_result_asserts.hpp" // checkedToolchainSkipped
#include "support/sema_support.hpp"       // parseAndCollect
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;
using bezel::test::SemaResult;

namespace {

// The class the parameter is borrowed over: a `readonly` field read after
// the chain call, so the row observes the object the UAF corrupted.
const char* kKlass =
    "public class K "
    "  public readonly N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "end K; ";

// The issue's TWO-LEVEL shape: `Use` returns a closure that invokes a
// closure that reads the borrowed parameter. 200 iterations amplify the
// report (one UAF per iteration) and the sum (1000) proves every read
// returned the right value.
std::string twoLevelProgram() {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Use(p: K): () -> K "
        "    var f := lambda() => p; "
        "    var g := lambda() => f(); "
        "    return lambda() => g(); "
        "  end Use; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var s2 := new K(5); "
        "      var r2 := Program.Use(s2)(); "
        "      t := t + r2.N; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

// The ONE-LEVEL twin: `Use` returns the closure that reads the parameter
// directly. Same defect, one lambda of nesting instead of two.
std::string oneLevelProgram() {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Use(p: K): () -> K "
        "    var f := lambda() => p; "
        "    return f; "
        "  end Use; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var s2 := new K(5); "
        "      var r2 := Program.Use(s2)(); "
        "      t := t + r2.N; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

// The LEGAL twin: the closure reads STATIC STORAGE instead of the borrowed
// parameter. Row 4's source set is then the static cell, the result is
// still a borrow, and the program is correct on r30-c1 too — EXCEPT that
// the pre-fix disposal freed the STATIC object, so the next iteration read
// freed memory. This row therefore pins that a borrowed chain-call result
// is never disposed, whichever source it borrows from.
std::string staticSourceProgram() {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static readonly _shared: K := new K(5); "
        "  public static method Use(): () -> K "
        "    return lambda() => Program._shared; "
        "  end Use; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var r := Program.Use()(); "
        "      t := t + r.N; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

// The BOUND twin of the issue's repro: the closure is bound to a local
// first and invoked through that local. Its callee is an identifier, so no
// chain-call disposal was ever registered for it.
//
// NOT A GTEST ROW. The program is GREEN on both arms — it is not red on the
// base, and no plausible wrong fix of THIS commit turns it red (the bound
// path never reaches the chain-call arm). `docs/ops/lane-contract.md` point 7
// says a green-on-both-arms program is not a gtest row but a CORPUS
// CANDIDATE, "put with that sentence under `corpus-candidates/` in your probe
// directory"; it is filed there as
// `/var/tmp/bezel-scratch/r43-m30-9040/probes/corpus-candidates/bound-chain-call-9040.bzl`
// (with the §28.3.5 row-4 sentence it comes from).

// The legal spelling of a chain call whose result the caller OWNS: the
// TARGET's function type carries the `owned` marker, so row 1 of the
// announcement table applies and the result is the caller's to dispose.
//
// The result is used as an UNBOUND TEMPORARY (not bound to a local): §28.3.11
// disposes an unbound owned temporary at the end of the full expression, and
// the CG60 arm's own temp-slot disposal is what does it. (Bound to a local,
// the same shape disposes the result TWICE — a separate, pre-existing defect,
// filed as #9082 — so this row uses the unbound form.)
std::string ownedUnboundProgram() {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Make(): () -> owned K "
        "    return lambda(): K => new K(5); "
        "  end Make; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      t := t + Program.Make()().N; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

// The issue's shape with the callee-side rule violated: the lambda mints a
// FRESH frame into a plain class/interface slot. Row 4's callee-side
// admission rejects it ("a fresh owned value rejected"), so the compiler
// must say so rather than accept a program whose frame nobody would ever
// dispose.
std::string freshIntoPlainSlotProgram() {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Make(): () -> K "
        "    return lambda(): K => new K(5); "
        "  end Make; "
        "  public static method Main(): int "
        "    var w := Program.Make()(); "
        "    return w.N; "
        "  end Main; "
        "end Program;";
}

// Lex + parse + resolve + typecheck, in the driver's own staging order
// (support/sema_support.hpp's own note on #3985/#3996).
SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const SemaResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The shared ASan oracle: the program ran, exited 0, printed the expected
// sum, and the sanitizer reported nothing. `runAotChecked` returns
// `ran == false` when the sanitizer fired (the report is carried in a
// diagnostic), so the `ran` assertion IS the "no memory error" oracle; the
// diagnostic scan names the failure mode if it ever fires.
void expectClean(const JitResult& r, const char* expectedStdout) {
    // The toolchain must be present: a skip is not a pass.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // No sanitizer report: the ASan memory error this issue is about always
    // lands here (and so does a leak, on the rows whose harness arms
    // LeakSanitizer).
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("AddressSanitizer fired") ==
                          std::string::npos);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

// The full net: LeakSanitizer AND the generated-code AddressSanitizer.
void expectCleanRun(const std::string& prog, const char* expectedStdout) {
    JitResult r = runAotChecked(prog);
    expectClean(r, expectedStdout);
}

// The LENIENT variant of the same oracle: LeakSanitizer off, the
// generated-code AddressSanitizer (the use-after-free this issue is about)
// still on.
//
// WHY THE LENIENT VARIANT IS NEEDED FOR SOME ROWS. The escaping closure's
// CAPTURE CELL leaks — a pre-existing, separate defect, present on r30-c1
// before this fix and in the BOUND form too (measured: 8 B per `Use` call,
// 1600 B over the 200 iterations below; the capture cell of the closure
// that escapes `Use` is never reclaimed). It is not this issue's subject,
// and LSan would report it as a leak and fail the row on a defect this
// commit does not touch. `runAotCheckedLenient` is the harness's sanctioned
// spelling for exactly that case ("tests whose program intentionally or
// incidentally leaks"); the memory-error net stays on, which is the oracle
// this issue needs.
void expectCleanRunLenient(const std::string& prog, const char* expectedStdout) {
    JitResult r = runAotCheckedLenient(prog);
    expectClean(r, expectedStdout);
}

}  // namespace

// Row 1 (RED on r30-c1): the issue's own repro, 200×. Pre-fix the
// chain-call result's scope-end disposal freed `s2`'s object and `s2`'s own
// scope-end disposal then read it — "ERROR: AddressSanitizer:
// heap-use-after-free … READ of size 8 … in main". LENIENT harness: this
// program's escaping closure leaks its capture cell (a pre-existing,
// separate defect — see the helper's note), so LeakSanitizer is off; the
// memory-error net that this issue needs is on.
BEZEL_TEST(returned_closure_chain_call_borrow_release_9040,
           two_level_chain_call_no_use_after_free_9040) {
    expectCleanRunLenient(twoLevelProgram(), "1000\n");
}

// Row 2 (RED on r30-c1): the one-level twin — the closure that reads the
// borrowed parameter directly. The same disposal freed `s2`'s object.
BEZEL_TEST(returned_closure_chain_call_borrow_release_9040,
           one_level_chain_call_no_use_after_free_9040) {
    expectCleanRun(oneLevelProgram(), "1000\n");
}

// Row 3 (RED on r30-c1): the legal static-storage twin. The pre-fix
// disposal freed the STATIC object, so the next iteration's read of it was
// the use-after-free — the same defect seen from its other side.
BEZEL_TEST(returned_closure_chain_call_borrow_release_9040,
           static_source_chain_call_no_use_after_free_9040) {
    expectCleanRun(staticSourceProgram(), "1000\n");
}

// Row 4 (BOUNDARY CONTROL — GREEN on r30-c1 and after the fix): the legal
// `owned` spelling is still ACCEPTED. The TARGET's function type carries the
// `owned` marker, so row 1 of the announcement table applies and the
// callee-side check is suppressed (§28.8's transferring slot). The sema
// companion must not reject it.
//
// THE PLAUSIBLE WRONG FIX THIS ROW IS RED UNDER: a callee-side check that
// ignores the `owned` marker — "a lambda whose target slot is a class/interface
// may not mint a fresh frame", with no §28.8 carve-out — rejects this
// program. (Named in the commit body, per `docs/ops/lane-contract.md` point 7.)
BEZEL_TEST(returned_closure_chain_call_borrow_release_9040,
           owned_target_spelling_still_accepted_9040) {
    auto r = parseSemaAll(ownedUnboundProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 5 (BOUNDARY CONTROL — GREEN on r30-c1 and after the fix): an OWNED
// chain-call result is still DISPOSED. The same program as row 4, run under
// the full net (LeakSanitizer armed): the 200 minted frames must all be
// reclaimed, or the row is RED.
//
// THE PLAUSIBLE WRONG FIX THIS ROW IS RED UNDER: a codegen gate that reads
// the ownership verdict from the slot's TYPE alone and forgets the `owned`
// marker — "a class/interface slot's chain-call result is never disposed" —
// leaks all 200 frames here. (Named in the commit body, per
// `docs/ops/lane-contract.md` point 7.)
BEZEL_TEST(returned_closure_chain_call_borrow_release_9040,
           owned_chain_call_result_still_disposed_9040) {
    expectCleanRun(ownedUnboundProgram(), "1000\n");
}

// Row 6 (RED on r30-c1): the callee-side rule itself. A lambda minting a
// FRESH frame into a plain class/interface slot is rejected by §28.3.5
// row 4 ("Lambda into such a slot: … a fresh owned value rejected") — the
// frame would be owned by nobody. Pre-fix the ASCRIPTION (`lambda(): K =>
// new K(5)`) was a way past the check, so the program compiled and its
// frame leaked; the ascription does not change the slot's kind, so the
// check now fires for it too.
BEZEL_TEST(returned_closure_chain_call_borrow_release_9040,
           fresh_owned_value_into_plain_slot_rejected_9040) {
    auto r = parseSemaAll(freshIntoPlainSlotProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "fresh owned value"));
}
