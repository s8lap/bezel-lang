// Regression tests for issue #8094 -- an UNBOUND OWNED STDLIB-FACTORY result
// is never released: it leaks at the §17.13 discard point (`_ := f();`) and at
// the §28.3 unbound-owned-temporary boundary (`f().Count`).
//
// -- WHAT THE DOCUMENTS REQUIRE ----------------------------------------------
// The stdlib declares the collection factories with an `owned` return:
//
//   stdlib §7.4  public static method GetFiles(path: string): owned Array<string>;
//   stdlib §7.4  public static method GetDirectories(path: string): owned Array<string>;
//   stdlib §7.3  public static method ReadAllLines(path: string): owned Array<string>;
//   stdlib §7.3  public static method ReadAllBytes(path: string): owned Array<byte>;
//   stdlib §2.4  public static method Variables(): owned Dictionary<string, string>;
//
// (§7.4 is `Directory`, §7.3 is `File`, §2.4 is `Environment`.)
//
// §28.3 "Unbound owned temporaries" (v0.355) says a value is one when it is
// "the result of an `owned`-returning call (or other owned producer) that is
// not bound to any name, not `move`d, and not returned", and that the rule is
// uniform across the source of the temporary:
//
//   "The `owned` return qualifier is the single signal that drives the reclaim;
//    there is no special-cased list of method names."
//
// §17.13 "The discard `_`" (v0.355) makes the DISCARD STATEMENT the discharge
// point of an owned result:
//
//   "**An owned result is disposed at the end of the discard statement.**"
//
// and §21.9 fixes the boundary of an expression statement: "end of the full
// expression" and "end of the statement" coincide there.
//
// -- THE DEFECT --------------------------------------------------------------
// `Emitter::isUnboundOwnedBorrowingArrayCallExpr`
// (`src/codegen/LlvmBackend_Setup.cpp`) is the predicate that admits an
// unbound owned CALL into `freeUnboundOwnedBorrowingArrayTemp`. It read the
// `owned` marker through `common::calleeReturnsOwned`, which resolves a USER
// `MethodSymbol`. A stdlib factory has NEITHER a `MethodDecl` NOR a
// `MethodSymbol` -- sema types those calls in its `colName`-keyed static arms
// with nothing attached to the callee -- so `calleeReturnsOwned` answered
// `false` for every one of them and the whole `owned`-returning stdlib
// surface was invisible to the reclaim.
//
// MEASURED on the parent of this fix (c18b2552), generated-code
// LeakSanitizer, 100 iterations each:
//
//   `_ := Directory.GetFiles("/tmp");`      -> 8 840 500 B / 285 600 allocs
//   `Directory.GetFiles("/tmp").Count`      -> 8 840 500 B / 285 600 allocs
//   `_ := File.ReadAllLines("/tmp");`       ->     7 920 B /     99 allocs
//   `_ := new Dictionary<string,string>();` -> CLEAN (the `new` face, #6357)
//
// The last line is the contrast that identifies the defect: the `new`-
// EXPRESSION face of an unbound owned temporary IS discharged
// (`isUnboundOwnedBuiltinCollectionNewExpr`), and the CALL face of one was not.
//
// -- THE FIX -----------------------------------------------------------------
// The same predicate now also admits a BUILTIN `owned` static factory, read
// through `common::isBuiltinOwnedStaticFactoryCall` -- the stdlib's own
// `owned`-factory table, which the ownership pass ALREADY consults to
// classify exactly these calls `Owned`. Codegen and the ownership pass
// therefore answer one question the same way, and no second copy of the
// factory table is spelled in codegen. The free itself is unchanged:
// `freeUnboundOwnedBorrowingArrayTemp`'s existing `isLL / isDict / else`
// dispatch, with the string-release thunk for an `Array<string>` (the co-owned
// +1 per element the `makeFilesList` / `bz_file_read_all_lines` lowerings
// mint) and the §28.11 VALUE-column disposer for a `Dictionary<string,string>`.
//
// -- THE ORACLE --------------------------------------------------------------
// Every leak row runs the real AOT path under the GENERATED-CODE sanitizer
// (`runAotChecked`: `opts.sanitizeAddress = true`), so a leak shows up as a
// LeakSanitizer report AND as a COUNT read out of LSan's own SUMMARY line
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO.
// Every leak row loops 100 times: a one-shot leak repro is a vacuous oracle.
//
// -- THE CONTROLS ------------------------------------------------------------
// Four shapes must stay GREEN on BOTH arms, so a row that pins behaviour is
// not confused with a row that pins a leak:
//
//   * `using var` -- the scope-bound disposal (the callee's own obligation);
//   * a plain `var` -- the §28.3 auto-drop at scope end;
//   * an OWNED PARAMETER PASS -- the callee owns the transferred result;
//   * a BORROWED (non-`owned`) stdlib return -- `File.Exists` is a `bool`.
//
// -- WHAT IS NOT HERE, AND WHY -----------------------------------------------
// * `File.ReadAllText` is NOT in this file. stdlib §7.3 declares it
//   `public static method ReadAllText(path: string): string;` -- NOT `owned`.
//   Its discarded result is a `string`, and §17.13 says of exactly that case:
//   "**Any other result makes the discard purely cosmetic.** For value types,
//   `string`, primitives, and the other runtime-managed types (§28.9) ...
//   no disposal is required", while §8.7 says "a `using var`, a discard or
//   scope end over a string disposes nothing". So a discarded `File.ReadAllText`
//   result is NOT an owned temporary and this fix does not (and must not)
//   reclaim it: that measurement is the general discarded-`string` question,
//   open as #6444 (a spec question, blocked on an owner ruling) and #6421.
//   MEASURED on this fix's parent: 3 200 B / 100 allocs -- 32 B per call, one
//   `bz.string` header, the +1 string producer's refcount.
// * The LINQ family (`Directory.GetFiles(d).Where(...)`) is NOT in this file
//   either: the runtime's `bz_lazy_own_source` frees an owned source with a
//   NULL element disposer ON PURPOSE (its own comment: "NULL element
//   disposer, and that is the SAFE direction rather than a shortcut"), so an
//   `Array<string>` LINQ source leaves its per-element +1 unreleased. That is
//   a PRE-EXISTING residual of the runtime's design, shared with the
//   user-`owned` twin: MEASURED, a user method declared
//   `Make(): owned Array<string>` (returning a fresh `+1` string) leaks the
//   SAME 3 500 B / 200 allocs through the same path.
//
// Cite: stdlib §2.4, §7.3, §7.4; language spec §8.7, §17.13, §21.9, §28.3,
// §28.11; v0.355.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"  // #5230: per-process @DATA@ paths
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// THE LEAK ORACLE. `leakedAllocations` is the COUNT of allocations
// LeakSanitizer reports as leaked: 0 when there is no SUMMARY line (LSan
// prints one only when it finds leaks), and the summary's own number when
// there is one. The pre-fix compiler produced 285 600 / 99 / 33 000 here; the
// fixed compiler produces 0.
//
// The COUNT is printed so a failing row's log carries the NUMBER the leak
// oracle measured.
void expectNoLeak(const JitResult& r, const std::string& out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string diag = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diag, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    // A SECOND, TRUNCATION-INDEPENDENT oracle. The COUNT above reads LSan's
    // SUMMARY line, which sits at the END of the report -- and the harness
    // caps the embedded report at 8 KB, so a row whose leak is huge (the
    // `Environment.Variables` row leaks one table PER ENVIRONMENT ENTRY PER
    // CALL: ~33 000 allocations) has its SUMMARY cut off and its COUNT read
    // as 0 even though the leak is real. The report's HEADLINE ("ERROR:
    // LeakSanitizer: detected memory leaks") is at the front and always
    // survives the cap, so this assertion is the one that fires for such a
    // row. Both oracles must hold on the fixed compiler: no report at all.
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasSanitizerReport(diag));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// Splice the per-process data directory into a program that writes files
// (#5230): two test processes running the same suite concurrently must not
// race on a FIXED path.
std::string withData(const std::string& program) {
    return bezel::test::spliceDataPaths(program, "bzt8094");
}

// Wrap a body fragment in a module + a class + a Main, with the setup
// fragment INSIDE Main (a fragment at the top level would not parse).
std::string program(const std::string& setup, const std::string& body) {
    return std::string("module M;\nclass P\npublic static method Main()\n") +
           setup + body + "end Main; end P;\n";
}

// The shape every leak row uses: a directory that EXISTS and holds two
// regular files and one subdirectory. `Directory.GetFiles` on it yields the
// two files, `Directory.GetDirectories` yields the one subdirectory.
//
// The directory is created by the program itself, so every row is
// self-contained and the file census is EXACT (the per-process @DATA@
// directory is shared by every row in this binary, so the rows read their own
// subdirectory and never the shared one).
std::string dirSetup(const std::string& dir) {
    return "Directory.CreateDirectory(\"@DATA@/" + dir + "\");\n"
           "File.WriteAllText(\"@DATA@/" + dir + "/f1.txt\", \"one\");\n"
           "File.WriteAllText(\"@DATA@/" + dir + "/f2.txt\", \"two\");\n"
           "Directory.CreateDirectory(\"@DATA@/" + dir + "/sub\");\n";
}

}  // namespace

// ===========================================================================
// THE §17.13 DISCARD POINT. `_ := Directory.GetFiles(dir);` -- the result is
// owned and unbound, and §17.13 discharges it "at the end of the discard
// statement".
//
// Pre-fix this read 285 600 allocations / 8 840 500 B (the array backing, its
// bucket block, and one `bz.string` header + data copy per entry). The fixed
// compiler reads 0.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           discard_of_directory_get_files_is_discharged_spec_17_13) {
    expectNoLeak(runAotChecked(withData(program(dirSetup("bzt8094_d1"), R"BZL(
var i := 0; while i < 100 do _ := Directory.GetFiles("@DATA@/bzt8094_d1"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// THE §28.3 UNBOUND-OWNED-TEMPORARY BOUNDARY. `Directory.GetFiles(dir).Count`
// -- the owned result is consumed as a RECEIVER and is owned by nothing, so
// §28.3 discharges it "at the end of the full expression in which it is
// produced".
//
// Pre-fix this read 285 600 allocations / 8 840 500 B -- the SAME shape as
// the discard above, which is what makes it a second boundary rather than a
// second defect.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           unbound_temporary_directory_get_files_count_is_discharged_spec_28_3) {
    expectNoLeak(runAotChecked(withData(program(dirSetup("bzt8094_d2"), R"BZL(
var n := 0; var i := 0; while i < 100 do n := n + Directory.GetFiles("@DATA@/bzt8094_d2").Count; i := i + 1; end; Console.WriteLine(n);
)BZL"))),
                 "200\n");
}

// ===========================================================================
// THE SIBLING `owned Array<string>` FACTORY: `File.ReadAllLines` (stdlib §7.3).
//
// Pre-fix this read 99 allocations / 7 920 B -- the array backing plus one
// `bz.string` per line.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           discard_of_file_read_all_lines_is_discharged_spec_17_13) {
    expectNoLeak(runAotChecked(withData(program(dirSetup("bzt8094_d3"), R"BZL(
var i := 0; while i < 100 do _ := File.ReadAllLines("@DATA@/bzt8094_d3/f1.txt"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// THE `owned Array<byte>` FACTORY: `File.ReadAllBytes` (stdlib §7.3). Its
// elements are bytes, so the backing free carries NO per-element disposer --
// the byte elements are the array's own storage.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           discard_of_file_read_all_bytes_is_discharged_spec_17_13) {
    expectNoLeak(runAotChecked(withData(program(dirSetup("bzt8094_d4"), R"BZL(
var i := 0; while i < 100 do _ := File.ReadAllBytes("@DATA@/bzt8094_d4/f1.txt"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// THE SIBLING DIRECTORY FACTORY: `Directory.GetDirectories` (stdlib §7.4).
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           discard_of_directory_get_directories_is_discharged_spec_17_13) {
    expectNoLeak(runAotChecked(withData(program(dirSetup("bzt8094_d5"), R"BZL(
var i := 0; while i < 100 do _ := Directory.GetDirectories("@DATA@/bzt8094_d5"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// THE `owned Dictionary<string, string>` FACTORY: `Environment.Variables`
// (stdlib §2.4, "returns a freshly-allocated `owned Dictionary<string,
// string>`"). The dict's VALUE column carries a +1 per stored value
// (§28.11), so the free must aim the string-release disposer at THAT column.
//
// Pre-fix this read 33 000 allocations / 1 852 600 B (one table, its bucket
// array and its insertion-order spine, plus one value `bz.string` per
// environment entry, per call).
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           discard_of_environment_variables_is_discharged_spec_17_13) {
    expectNoLeak(runAotChecked(withData(program("", R"BZL(
var i := 0; while i < 100 do _ := Environment.Variables(); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// THE THIRD BOUNDARY: §28.3 K8 with §21.9's "end of the full expression" ==
// "end of the statement". A BARE expression statement
// (`Directory.GetFiles(dir);`) is the same unbound owned temporary in the
// same statement position as the `_ :=` form.
//
// The §21.9 unused-result warning is EXPECTED here (the statement's result
// is deliberately unused); it is a warning, not an error.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           bare_statement_of_directory_get_files_is_discharged_spec_28_3) {
    expectNoLeak(runAotChecked(withData(program(dirSetup("bzt8094_d6"), R"BZL(
var i := 0; while i < 100 do Directory.GetFiles("@DATA@/bzt8094_d6"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// CONTROL 1 -- `using var`: the scope-bound disposal. GREEN on BOTH arms: the
// result is BOUND, so it is never an unbound owned temporary, and the
// `using var`'s own scope-end disposal is the sole free.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           using_var_binding_stays_clean_on_both_arms_spec_28_3) {
    expectNoLeak(runAotChecked(withData(program(dirSetup("bzt8094_c1"), R"BZL(
var i := 0; while i < 100 do using var d := Directory.GetFiles("@DATA@/bzt8094_c1"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// CONTROL 2 -- a plain `var`: the §28.3 auto-drop at scope end. GREEN on
// BOTH arms. The auto-drop warning is EXPECTED (the value is owned and
// implicitly disposed at the end of its scope).
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           plain_var_binding_stays_clean_on_both_arms_spec_28_3) {
    expectNoLeak(runAotChecked(withData(program(dirSetup("bzt8094_c2"), R"BZL(
var i := 0; while i < 100 do var d := Directory.GetFiles("@DATA@/bzt8094_c2"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// CONTROL 3 -- an OWNED PARAMETER PASS: the result is TRANSFERRED to the
// callee, which owns it and disposes it. GREEN on BOTH arms, and it is the
// shape a widened reclaim could most easily double-free: the call site must
// NOT also free what it handed over.
//
// The callee reads the array (so the transfer is real, not a dead store) and
// returns its count.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           owned_parameter_pass_stays_clean_on_both_arms_spec_28_3) {
    const std::string p = withData(
        "module M;\nclass P\n"
        "public static method Take(owned a: Array<string>): int\n"
        "  return a.Count;\n"
        "end Take;\n"
        "public static method Main()\n" +
        dirSetup("bzt8094_c3") +
        "var n := 0; var i := 0; while i < 100 do n := n + Take(Directory.GetFiles(\"@DATA@/bzt8094_c3\")); i := i + 1; end; Console.WriteLine(n);\n"
        "end Main; end P;\n");
    expectNoLeak(runAotChecked(p), "200\n");
}

// ===========================================================================
// CONTROL 4 -- a BORROWED (non-`owned`) stdlib return. `File.Exists` is
// declared `public static method Exists(path: string): bool;` (stdlib §7.3) --
// a primitive, not an owned production. GREEN on BOTH arms: the widened
// reclaim must not touch it.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           borrowed_stdlib_return_stays_clean_on_both_arms_spec_28_3) {
    expectNoLeak(runAotChecked(withData(program("", R"BZL(
var i := 0; while i < 100 do _ := File.Exists("@DATA@/bzt8094_c4_no_such_file"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// CONTROL 5 -- the `new`-EXPRESSION face of an unbound owned temporary, which
// #6357 already discharges (`isUnboundOwnedBuiltinCollectionNewExpr`). GREEN
// on BOTH arms: it is the control that shows the CALL face was the gap.
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           new_expression_face_stays_clean_on_both_arms_spec_28_3) {
    expectNoLeak(runAotChecked(withData(program("", R"BZL(
var i := 0; while i < 100 do _ := new Dictionary<string, string>(); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// ===========================================================================
// THE BEHAVIOUR CONTROL: the fix must not change WHAT the factories compute.
// `Directory.GetFiles` yields each entry's FULL path (stdlib §7.4 / the
// runtime's `readdir` order, which the spec does not fix), so the rows pin
// the COUNT and the MEMBERSHIP, not the order.
//
// This row runs on the plain `runAot` oracle: its subject is the VALUES, not
// the leak (the leak rows above carry the count).
// ===========================================================================
BEZEL_TEST(unbound_owned_stdlib_factory_8094,
           directory_get_files_result_is_correct_spec_7_4) {
    auto r = runAot(withData(program(dirSetup("bzt8094_b1"), R"BZL(
using var files := Directory.GetFiles("@DATA@/bzt8094_b1");
Console.WriteLine(files.Count);
if files.Contains("@DATA@/bzt8094_b1/f1.txt") then Console.WriteLine(1); else Console.WriteLine(0); end;
if files.Contains("@DATA@/bzt8094_b1/f2.txt") then Console.WriteLine(1); else Console.WriteLine(0); end;
if files.Contains("@DATA@/bzt8094_b1/sub") then Console.WriteLine(0); else Console.WriteLine(1); end;
)BZL")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n1\n1\n1\n"));
}
