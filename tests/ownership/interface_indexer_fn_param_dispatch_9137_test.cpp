// Regression test for issue #9137 — a read of an indexer whose parameter is
// FUNCTION-TYPED, through an INTERFACE-typed variable, SEGV'd: the interface
// dispatch of the indexer getter/setter called a NULL itable slot (address
// 0x0), because the implementing class's indexer was never matched against
// the interface's.
//
// THE DEFECT. `Emitter::findIndexer` (src/codegen/LlvmBackend_Assign.cpp) is
// the codegen mirror of sema's interface-conformance check: it selects the
// class indexer that implements an interface indexer by comparing their
// parameter TYPE LISTS (§23.4: "Indexers may be overloaded by parameter type
// list"), and `declareVtables` Pass D installs the selected accessor's
// function pointer into the class's itable slot for that interface. The
// comparison this file used (`indexerArgMatchesUnder`) answered `false` for
// every kind it did not spell out — `TypeRef::Kind::Function` among them — so
// a class indexer with a function-typed parameter NEVER matched the
// interface's. `findIndexer` returned null, the itable slot kept its
// `ConstantPointerNull` initialiser, and the interface-typed dispatch called
// address 0x0.
//
// MEASURED on r30-c1 @ 84e3f112d (`bezelc --sanitize=address --link=system`):
//
//   AddressSanitizer:DEADLYSIGNAL
//   ==1952964==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000
//   (pc 0x000000000000 bp 0x7ffdd1e1abb0 sp 0x7ffdd1e1ab18 T0)
//   ==1952964==HINT: pc points to the zero page.
//
// for the getter, the setter and the compound assignment alike. The IR shows
// the null slot directly (`BEZEL_DUMP_IR=1`):
//
//   @"itab$M1...FlI$M1...IFl" = internal constant [2 x ptr]
//        [ptr null, ptr inttoptr (i64 1 to ptr)]
//   ...
//   %idxget = load ptr, ptr %12, align 8
//   %21 = call i32 %idxget(ptr %8, ptr %13)      ; -> calls address 0x0
//
// while the SAME program with an `int`-parameter indexer emits
// `[ptr @"bz$indexer$get", ptr inttoptr (i64 1 to ptr)]` and answers 7.
//
// THE TWINS THAT ARE ALREADY CORRECT (rows 4 and 5) — the class-typed read of
// the very same function-typed indexer, and the interface-typed read of an
// `int`-parameter indexer. The defect is specific to a FUNCTION-typed index
// parameter read through an INTERFACE-typed receiver.
//
// THE SPEC. §11.2 (v0.359), interface declaration: an interface's members are
// matched by the implementing class's members, and a call through an
// interface-typed receiver dispatches to the implementation's own member.
// §23.4 (v0.359), Indexers: "Indexers may be declared in interfaces with the
// body-less form … Indexers may be overloaded by parameter type list." §27.1
// (v0.359): a function type is a first-class type, so an indexer's parameter
// may be function-typed — and §23.5 (v0.359) forbids only `ref`/`out` on an
// indexer parameter list, leaving a plain (call-bounded, §28.14) callback
// parameter admitted.
//
// THE CHANGE. Two sites, both on the interface dispatch of an indexer:
//
//   1. `indexerParamTypeMatchesUnder` (new, LlvmBackend_Assign.cpp) — the
//      FUNCTION-typed arm of the parameter-type comparison, the codegen twin
//      of sema's `Resolver::typeRefEqualImpl`'s `Kind::Function` arm
//      (src/sema/TypeResolution_Checks.cpp): the arity, each parameter's
//      modifier and type, and the return's ownedness and type. `findIndexer`
//      now compares through it, so a class indexer with a function-typed
//      parameter matches its interface obligation and the itable slot is
//      filled with the implementation's accessor.
//
//   2. The closure-producer release on the interface dispatch (the getter,
//      the setter and the compound assignment). §28.14 (v0.359): a
//      function-typed parameter declared without `scoped` is CALL-BOUNDED —
//      "the callee receives the callback for the duration of the call only" —
//      so the dispatched accessor keeps no reference to the callback after it
//      returns, and the producer's +1 is the CALLER's to drop; §28.9
//      (v0.359): the closure object is runtime-managed, so its own reference
//      must be dropped exactly once. The interface dispatch did not discharge
//      it — the path was UNREACHABLE before the fix above (the slot was null
//      and the dispatch crashed), so the leak was masked by the crash. With
//      the dispatch working it shows: MEASURED on r30-c1 + the dispatch fix,
//      64 byte(s) in 1 allocation per access (the closure object alone).
//      This is the twin of #9068's vtable-dispatch release.
//
// THE ORACLE. Every row runs the program TWICE: once plain (`runAot`) for the
// VALUE oracle — the program's own exit code and output — and once checked
// (`runAotChecked`) for the MEMORY oracle.
//
// ROWS.
//   Row 1 — the issue's own shape: the interface-typed READ of a function-
//           typed indexer, 200 iterations. RED on the base (the SEGV), GREEN
//           with the fix (1400, no leak).
//   Row 2 — the SETTER shape: `fl[lambda() => 7] := 3;` then a read, 200
//           iterations. RED on the base (the SEGV), GREEN with the fix (3400,
//           no leak).
//   Row 3 — the COMPOUND shape: `fl[lambda() => 7] += 1;` then a read, 200
//           iterations. RED on the base (the SEGV), GREEN with the fix (4400,
//           no leak).
//   Row 4 — GREEN CONTROL: the CLASS-typed read of the very same indexer
//           (the twin the issue says already works). Green on both arms; it
//           is the guard that the fix does not change the class-typed
//           dispatch.
//   Row 5 — GREEN CONTROL: the interface-typed read of an `int`-parameter
//           indexer. Green on both arms; it is the guard that the fix does
//           not change the non-function-typed interface dispatch.
//
// per §11.2, §23.4, §23.5, §27.1, §28.9, §28.14 (issue #9137)

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

// The FUNCTION-typed indexer family: an interface declaring an indexer over a
// `() -> int` parameter, a class implementing it (its getter returns
// `_last + q()`, its setter stores `q() + value`), and a `Main` that loops
// 200 times.
//
// `cls` names the module/class stem, `ifaceDecl` the interface's indexer
// declaration (`readonly property [q: () -> int]: int;` for the getter-only
// shape, `property [q: () -> int]: int;` for the read-write one), `stmt` the
// statement(s) executed per iteration and `read` the expression whose value
// is accumulated.
//
// The arithmetic (why the expected outputs are 1400 / 3400 / 4400): the
// getter returns `_last + q()` and the lambda returns 7, so a read is 7 on
// the first iteration of a fresh instance; the setter stores `q() + value`.
//   * read only                       -> 0 + 7 = 7            -> 200 * 7  = 1400
//   * `:= 3` then read                -> _last = 7 + 3 = 10,
//                                       read = 10 + 7 = 17    -> 200 * 17 = 3400
//   * `+= 1` then read                -> 7 + 1 = 8, setter stores
//                                       _last = 7 + 8 = 15,
//                                       read = 15 + 7 = 22    -> 200 * 22 = 4400
std::string fnParamProgram(const std::string& cls,
                            const std::string& ifaceDecl,
                            const std::string& stmt,
                            const std::string& read) {
    return "module " + cls + "; "
           "public interface IFl" + cls + " "
           "  " + ifaceDecl + " "
           "end IFl" + cls + "; "
           "public class FlI" + cls + " : IFl" + cls + " "
           "  private _last: int := 0; "
           "  public property [q: () -> int]: int "
           "    get => _last + q(); "
           "    set => do _last := q() + value; end; "
           "  end; "
           "  public constructor() end constructor; "
           "end FlI" + cls + "; "
           "public class Program" + cls + " "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do "
           "      using var c := new FlI" + cls + "(); "
           "      var fl: IFl" + cls + " := c; "
           "      " + stmt + " "
           "      t := t + " + read + "; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program" + cls + ";";
}

// ── Row 1 (§11.2 / §23.4 / §27.1): the issue's own shape — the
// interface-typed READ of a function-typed indexer, 200 iterations.
//
// RED on the base (the SEGV at address 0x0), GREEN with the fix (1400, and
// the checked run leaks nothing: the call-bounded callback's producer is
// discharged at the call's end).
BEZEL_TEST(interface_indexer_fn_param_dispatch_9137,
           interface_typed_read_of_a_function_typed_indexer_dispatches) {
    const std::string src = fnParamProgram(
        "Fn9137R", "readonly property [q: () -> int]: int;",
        "", "fl[lambda() => 7]");
    auto v = runAot(src);
    expectValue(v, 0, "1400\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 2 (§11.2 / §23.4 / §23.5 / §28.14): the SETTER shape —
// `fl[lambda() => 7] := 3;` through the interface-typed variable, then a
// read. 200 iterations.
//
// RED on the base (the SEGV), GREEN with the fix (3400, no leak).
BEZEL_TEST(interface_indexer_fn_param_dispatch_9137,
           interface_typed_write_of_a_function_typed_indexer_dispatches) {
    const std::string src = fnParamProgram(
        "Fn9137S", "property [q: () -> int]: int;",
        "fl[lambda() => 7] := 3;", "fl[lambda() => 7]");
    auto v = runAot(src);
    expectValue(v, 0, "3400\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 3 (§6.6 / §23.4 / §28.14): the COMPOUND shape —
// `fl[lambda() => 7] += 1;` through the interface-typed variable, then a
// read. 200 iterations.
//
// RED on the base (the SEGV), GREEN with the fix (4400, no leak).
BEZEL_TEST(interface_indexer_fn_param_dispatch_9137,
           interface_typed_compound_write_of_a_function_typed_indexer_dispatches) {
    const std::string src = fnParamProgram(
        "Fn9137C", "property [q: () -> int]: int;",
        "fl[lambda() => 7] += 1;", "fl[lambda() => 7]");
    auto v = runAot(src);
    expectValue(v, 0, "4400\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 4 (GREEN CONTROL — the CLASS-typed twin): the very same function-
// typed indexer, read through the CLASS-typed variable `c`.
//
// Green on the base and green with the fix: the issue says the class-typed
// read already works, and this row is the guard that the fix does not change
// it (a fix that routed the class-typed access through the itable would
// turn this row red).
BEZEL_TEST(interface_indexer_fn_param_dispatch_9137,
           class_typed_read_of_the_same_indexer_is_unchanged) {
    const std::string src = fnParamProgram(
        "Fn9137K", "readonly property [q: () -> int]: int;",
        "", "c[lambda() => 7]");
    auto v = runAot(src);
    expectValue(v, 0, "1400\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 5 (GREEN CONTROL — the `int`-parameter twin): an interface indexer
// over an `int` parameter, read through the interface-typed variable.
//
// Green on the base and green with the fix: the issue asks whether an
// `int`-parameter interface indexer read also crashes (it does not), and this
// row is the guard that the fix does not change the non-function-typed
// interface dispatch.
BEZEL_TEST(interface_indexer_fn_param_dispatch_9137,
           interface_typed_read_of_an_int_parameter_indexer_is_unchanged) {
    const std::string src =
        "module Fn9137I; "
        "public interface IFlFn9137I "
        "  readonly property [i: int]: int; "
        "end IFlFn9137I; "
        "public class FlIFn9137I : IFlFn9137I "
        "  private _last: int := 0; "
        "  public property [i: int]: int => _last + i; "
        "  public constructor() end constructor; "
        "end FlIFn9137I; "
        "public class ProgramFn9137I "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      using var c := new FlIFn9137I(); "
        "      var fl: IFlFn9137I := c; "
        "      t := t + fl[7]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end ProgramFn9137I;";
    auto v = runAot(src);
    expectValue(v, 0, "1400\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

}  // namespace
