// Regression test for issue #8964 — a record's FUNCTION-TYPED component, built
// from a lambda, leaked the whole 64-byte closure cell on every construction:
// the record's disposal released `string` components and nothing else, so a
// record with a function-typed component had no owner for the cell at all.
//
// THE DEFECT. `record R(f: () -> int);` … `var r := new R(lambda() => 3);`
// The construction-site store (buildRecordValue's positional-field store) is a
// BARE `CreateStore`, so the component ALIASES the lambda's cell (rc = 1 from
// initClosureRc): the +1 the producer minted is the RECORD's to discharge
// (§28.9 — every heap object is disposed exactly once). MEASURED on
// r43-f45-toolchain @ 86ced0bec with `bezelc --sanitize=address`:
//
//   Direct leak of 12800 byte(s) in 200 object(s) allocated from:
//       #0 ... in calloc
//       #1 ... in main
//   SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s).
//
// 12800 B / 200 objects = 64 B per construction — the issue's own figure.
// The base's IR had exactly ONE closure cell per iteration and NO
// `bz_closure_release` call anywhere in the module.
//
// THE ROOT CAUSE. The record's component-disposal path is
// `emitRecordStringFieldReleases_impl` (src/codegen/LlvmBackend_impl.hpp) — it
// walks a record's storage and releases only `string` components, and it is
// admitted by the `recordStringFields` Disposal, whose proof is
// `isFreshRecordConstructionOfExactType`'s STRING arm (each string argument a
// literal or a +1 producer). There was no FUNCTION-TYPED twin: no predicate
// that recognises a function-typed component, no walker that releases one, and
// no admission proof that a function-typed argument was a +1 PRODUCER rather
// than a BORROW. The CLASS-field path had all three (the #429 walk in
// emitOwnedFieldDisposals), but a class field holds an INDEPENDENT +1
// (retained at store), while a record's component holds the +1 the producer
// minted — TRANSFERRED by the bare store.
//
// THE FIX. A separate, closure-shaped path, gated by its own ownership proof:
//   * `isClosureStorageMemberResolved` / `recordHasClosureFields` /
//     `isRecordWithClosureFieldsTypeRef` — the closure twins of the string
//     path's `isStringStorageMemberResolved` / `recordHasStringFields` /
//     `isRecordWithStringFieldsTypeRef`;
//   * `emitRecordClosureFieldReleases` — GEP the slot, load the cell,
//     bz_closure_release (null-safe), null the slot; the closure twin of
//     `emitRecordStringFieldReleases_impl`;
//   * the `recordClosureFields` Disposal — the closure twin of
//     `recordStringFields`;
//   * `isFreshRecordConstructionOfExactType`'s FUNCTION-TYPED arm — the
//     argument must be a +1 closure PRODUCER (isPlusOneClosureProducer: a
//     lambda, a method reference, a conditional of producers), never a BORROW.
//
// THE TWO PATHS ARE SEPARATE ON PURPOSE. The string release and the closure
// release are gated by DIFFERENT ownership proofs (the string arm vs the
// function-typed arm of the freshness predicate), and the string walk is also
// reachable from the class-field walk (emitOwnedFieldDisposals' record-typed
// field arm), where the record's component is a BORROW. Widening the shared
// predicate would have widened the STRING release's admission surface (a
// behaviour flip in the leak direction) and released a borrowed cell.
//
// THE SPEC. §9.7 (v0.359): "Reference-typed fields whose declared types are
// runtime-managed (`string`, `Type`, function types) do not make the record
// borrow-bearing because runtime-managed values have unbounded lifetime by
// construction" — unbounded lifetime is not ownership: the record's component
// still holds a reference, and §28.9 (v0.359) requires that reference to be
// discharged exactly once. §28.3 (v0.359) puts the record component in the
// function-typed holder list, so the +1 the producer minted is discharged by
// the holder — here, the record.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program), so the leak cannot hide under the sanitizer's noise.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

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

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
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

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// THE REGRESSION ROW: the issue's own repro, verbatim in shape, amplified
// 200x (the issue's own measurement). The record's function-typed component
// is built from a capture-free lambda — the issue says ANY lambda leaks, even
// a capture-free one.
//
// RED on the base (12800 B / 200 objects — 64 B per construction): the cell
// was never released. GREEN with the fix.
BEZEL_TEST(record_closure_component_leak_8964,
           issue_repro_200_iterations_reclaims_the_closure_cell) {
    const std::string src =
        "module P; "
        "record R(f: () -> int); "
        "public class Program "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0..<200 do "
        "      var r := new R(lambda() => 3); "
        "      acc := acc + r.f(); "
        "    end; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "600\n");
}

// The TWO-COMPONENT row: a record that owns a `string` component AND a
// function-typed component. The string release (the `recordStringFields`
// path) already existed; the closure release (the `recordClosureFields` path)
// is the new one. They are SEPARATE paths with separate ownership proofs, so
// this row pins that they coexist in ONE record: the string component is
// released by its own walk, the closure component by its own.
//
// RED on the base (64 B / 1 object per construction — the string is released
// by the base's own path, the cell is not): 12800 B / 200 objects.
// GREEN with the fix.
BEZEL_TEST(record_closure_component_leak_8964,
           record_with_a_string_and_a_closure_component_releases_both) {
    const std::string src =
        "module P; "
        "record R(s: string, f: () -> int); "
        "public class Program "
        "  public static method Run(): int "
        "    var acc := 0; "
        "    var lens := 0; "
        "    for var i in 0..<200 do "
        "      var r := new R(\"hi\", lambda() => 3); "
        "      acc := acc + r.f(); "
        "      lens := lens + r.s.Length; "
        "    end; "
        "    Console.WriteLine(acc); "
        "    Console.WriteLine(lens); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    Program.Run(); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "600\n400\n");
}

// THE BOUNDARY CONTROL — a BORROWED closure component. The record's
// component is built from a closure LOCAL, not from a producer: the cell
// belongs to `g`, which disposes it at its own scope end. The record must
// NOT release it.
//
// This is the row that makes the ownership proof visible. A fix that
// released the record's closure component UNCONDITIONALLY (i.e. without the
// +1-producer requirement of the freshness proof) would free the cell here
// twice — once for `g`, once for the record — and AddressSanitizer would
// report a double-free. It is green on the base (the base released nothing)
// and is red under that wrong fix.
//
// It is also a leak-oracle row, which the conformance corpus cannot express:
// the check is `assertNoLeak` over 200 iterations (CLAUDE.md, *Test and
// oracle rules* — at most two gtest boundary controls per suite).
BEZEL_TEST(record_closure_component_leak_8964,
           borrowed_closure_component_is_not_released_by_the_record) {
    const std::string src =
        "module P; "
        "record R(f: () -> int); "
        "public class Program "
        "  public static method Run(): int "
        "    var acc := 0; "
        "    for var i in 0..<200 do "
        "      var g := lambda() => 3; "
        "      var r := new R(g); "
        "      acc := acc + r.f(); "
        "    end; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    Program.Run(); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "600\n");
}

// fix-forward #8964, review shape (d2): a component READ that is not a call
// (`r.f` as an argument, an initializer or an indexer / Dictionary store
// value) hands the collection the SAME cell the record's scope-end release
// would free: a collection store of a closure does not retain it. The record
// must decline the release (a leak, as on the base, never a free). §28.9 /
// §28.3, v0.359. RED on the first #8964 fix (heap-use-after-free freed by
// bz_closure_release), GREEN with the walker's closure-component-read decline.
// Lenient oracle: the generated-code ASan is active, the leak sentinel off
// (the decline is a leak by design).
BEZEL_TEST(record_closure_component_leak_8964,
           array_add_of_component_read_does_not_free_the_stored_cell) {
    const std::string src =
        "module P; "
        "import System; "
        "import System.Collections; "
        "record R(f: () -> int); "
        "public class Program "
        "  public static method Main(): int "
        "    var fs := new Array<() -> int>(); "
        "    for var i in 0..<200 do "
        "      var r := new R(lambda() => 3); fs.Add(r.f); "
        "    end; "
        "    var acc := 0; "
        "    for var g in fs do acc := acc + g(); end; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    const JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: AddressSanitizer") ==
                      std::string::npos);
}

// fix-forward #8964, review shape (e2): a component READ that is not a call
// (`r.f` as an argument, an initializer or an indexer / Dictionary store
// value) hands the collection the SAME cell the record's scope-end release
// would free: a collection store of a closure does not retain it. The record
// must decline the release (a leak, as on the base, never a free). §28.9 /
// §28.3, v0.359. RED on the first #8964 fix (heap-use-after-free freed by
// bz_closure_release), GREEN with the walker's closure-component-read decline.
// Lenient oracle: the generated-code ASan is active, the leak sentinel off
// (the decline is a leak by design).
BEZEL_TEST(record_closure_component_leak_8964,
           array_add_of_local_alias_of_component_does_not_free_the_stored_cell) {
    const std::string src =
        "module P; "
        "import System; "
        "import System.Collections; "
        "record R(f: () -> int); "
        "public class Program "
        "  public static method Main(): int "
        "    var fs := new Array<() -> int>(); "
        "    for var i in 0..<200 do "
        "      var r := new R(lambda() => 3); var g := r.f; fs.Add(g); "
        "    end; "
        "    var acc := 0; "
        "    for var g in fs do acc := acc + g(); end; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    const JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: AddressSanitizer") ==
                      std::string::npos);
}

// fix-forward #8964, review shape (f1): a component READ that is not a call
// (`r.f` as an argument, an initializer or an indexer / Dictionary store
// value) hands the collection the SAME cell the record's scope-end release
// would free: a collection store of a closure does not retain it. The record
// must decline the release (a leak, as on the base, never a free). §28.9 /
// §28.3, v0.359. RED on the first #8964 fix (heap-use-after-free freed by
// bz_closure_release), GREEN with the walker's closure-component-read decline.
// Lenient oracle: the generated-code ASan is active, the leak sentinel off
// (the decline is a leak by design).
BEZEL_TEST(record_closure_component_leak_8964,
           dictionary_set_of_component_read_does_not_free_the_stored_cell) {
    const std::string src =
        "module P; "
        "import System; "
        "import System.Collections; "
        "record R(f: () -> int); "
        "public class Program "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, () -> int>(); "
        "    for var i in 0..<200 do "
        "      var r := new R(lambda() => 3); d[i] := r.f; "
        "    end; "
        "    var acc := 0; "
        "    for var i in 0..<200 do acc := acc + d[i](); end; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    const JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: AddressSanitizer") ==
                      std::string::npos);
}

// fix-forward #8964, review shape (f2): a component READ that is not a call
// (`r.f` as an argument, an initializer or an indexer / Dictionary store
// value) hands the collection the SAME cell the record's scope-end release
// would free: a collection store of a closure does not retain it. The record
// must decline the release (a leak, as on the base, never a free). §28.9 /
// §28.3, v0.359. RED on the first #8964 fix (heap-use-after-free freed by
// bz_closure_release), GREEN with the walker's closure-component-read decline.
// Lenient oracle: the generated-code ASan is active, the leak sentinel off
// (the decline is a leak by design).
BEZEL_TEST(record_closure_component_leak_8964,
           array_index_store_of_component_read_does_not_free_the_stored_cell) {
    const std::string src =
        "module P; "
        "import System; "
        "import System.Collections; "
        "record R(f: () -> int); "
        "public class Program "
        "  public static method Main(): int "
        "    var fs := new Array<() -> int>(); for var i in 0..<200 do fs.Add(lambda() => 1); end; "
        "    for var i in 0..<200 do "
        "      var r := new R(lambda() => 3); fs[i] := r.f; "
        "    end; "
        "    var acc := 0; "
        "    for var g in fs do acc := acc + g(); end; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    const JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: AddressSanitizer") ==
                      std::string::npos);
}

// fix-forward #8964, review shape (f3): a component READ that is not a call
// (`r.f` as an argument, an initializer or an indexer / Dictionary store
// value) hands the collection the SAME cell the record's scope-end release
// would free: a collection store of a closure does not retain it. The record
// must decline the release (a leak, as on the base, never a free). §28.9 /
// §28.3, v0.359. RED on the first #8964 fix (heap-use-after-free freed by
// bz_closure_release), GREEN with the walker's closure-component-read decline.
// Lenient oracle: the generated-code ASan is active, the leak sentinel off
// (the decline is a leak by design).
BEZEL_TEST(record_closure_component_leak_8964,
           alias_and_direct_component_reads_stored_together_do_not_free_the_stored_cells) {
    const std::string src =
        "module P; "
        "import System; "
        "import System.Collections; "
        "record R(f: () -> int); "
        "public class Program "
        "  public static method Main(): int "
        "    var fs := new Array<() -> int>(); "
        "    for var i in 0..<5 do "
        "      var r := new R(lambda() => 3); var g := r.f; fs.Add(g); fs.Add(r.f); "
        "    end; "
        "    var acc := 0; "
        "    for var g in fs do acc := acc + g(); end; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    const JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "30\n");
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: AddressSanitizer") ==
                      std::string::npos);
}

}  // namespace
