// Regression tests for #8024 -- `Directory.GetFiles` / `Directory.GetDirectories`
// leak the runtime's `char**` RETURN BUFFER and its `strdup`'d ENTRIES.
//
// MEASURED on the R38 staging tip `a030f321` (the #8016 tip -- pre-fix for
// THIS issue; the reviewer measured the same numbers):
//
//   `Directory.GetFiles(<dir with 2 files>)` 100x
//       -> 300 allocations / 11600 B   (100 `char**` arrays + 200 entries)
//   `Directory.GetDirectories(<dir with 1 subdir>)` 100x
//       -> 200 allocations /  N B      (100 `char**` arrays + 100 entries)
//   `Directory.GetFiles(<empty dir>)` 100x
//       -> 100 allocations /  N B      (100 `char**` arrays, no entries)
//
// -- THE DEFECT --------------------------------------------------------------
// stdlib §7.4 declares
//
//     public static method GetFiles(path: string): owned Array<string>;
//     public static method GetDirectories(path: string): owned Array<string>;
//
// and the codegen lowers them by calling the runtime's
// `bz_dir_get_files` / `bz_dir_get_dirs`, which return a `char**`:
//
//     char** arr = (char**)malloc(results.size() * sizeof(char*));
//     for (size_t i = 0; i < results.size(); i++) arr[i] = results[i];
//
// with every `results[i]` a `strdup`'d copy of one entry's full path. The
// codegen's `makeFilesList` arm (LlvmBackend_Calls.cpp) COPIES each entry into
// a fresh `bz.string` payload (`memcpy`) and then dropped the C strings on the
// floor: neither the `char**` nor its entries were ever released. This is the
// THIRD defect in the same arms (the #7865 `Path.*` family and the #8016
// `File.*` / `Directory.*` `strToPath` copies), and it is invisible for a
// directory that does NOT exist -- the runtime returns `nullptr` and allocates
// nothing there.
//
// -- THE OWNERSHIP RULE (why the CALLER releases them) ------------------------
// The `char**` and its entries are the CALLER's to release, and the rule is
// established by the runtime's own code, not assumed:
//
//   * `bz_dir_get_files` / `bz_dir_get_dirs`
//     (src/runtime/bezel_runtime_fileio.cpp) `malloc` the array and `strdup`
//     each entry into a LOCAL `std::vector<char*> results` that dies at the
//     `return`. After the call the runtime holds NO handle on either the
//     array or its entries, so there is NO runtime-side reclamation point for
//     them: the runtime cannot free them later even in principle. They are a
//     hand-over, not a runtime-owned arena.
//   * Every entry is DEAD once the arm has copied it into the fresh
//     `bz.string` payload (`memcpy` -- the copy is independent), and the
//     `char**` is DEAD once the copy loop ends. The lowering arm is the only
//     party still holding them.
//   * The allocator matches: the runtime allocates with libc `malloc` /
//     `strdup` and the arm releases with libc `free` -- same allocator, same
//     process.
//
// The SPEC's part is §28.9 (v0.350), NOT §28.3: "How the runtime manages
// their lifecycle (reference counting, region allocation, or other) is not part
// of this specification", and "the implementation reclaims the production".
// The `char**` is the implementation's own scratch on the way to the
// `owned Array<string>` production stdlib §7.4 declares, so §28.9 fixes the
// OUTCOME (the implementation reclaims it) and leaves the mechanism to the
// implementation -- and the implementation's own code above decides it. (§28.3
// is NOT the ground: the producers table's "any call" row reads "not
// applicable" for a runtime-managed result, so no obligation arises there.)
//
// -- THE LEAK ORACLE ---------------------------------------------------------
// A row that asserted only the program's OUTPUT would be GREEN on the pre-fix
// code and would prove nothing. Every LEAK row here measures a COUNT: the
// number of allocations LeakSanitizer reports as leaked, read out of LSan's
// own "SUMMARY: AddressSanitizer: <bytes> byte(s) leaked in <allocations>
// allocation(s)." line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO;
// on the pre-fix compiler the same rows read 300 / 200 / 100. The rows also
// run under `runAotChecked`, whose LSan leg turns a leak into a failed row, so
// the count and the verdict agree.
//
// Every leak row loops 100 times: a one-shot leak repro is a vacuous oracle.
//
// -- THE POSITIVE CONTROLS ---------------------------------------------------
// Three rows must be GREEN on BOTH arms, so a row that pins behaviour is not
// confused with a row that pins a leak:
//
//   * the missing-directory row (the runtime's `nullptr` return: the release
//     path must not crash on it, and it leaks nothing on either arm);
//   * the behaviour rows (the VALUES `Directory.GetFiles` / `GetDirectories`
//     compute must not change: a "fix" that silenced the leak by returning
//     the wrong answer would be RED here).
//
// -- WHAT IS NOT HERE, AND WHY -----------------------------------------------
// * A +1 string PRODUCER passed as an ARGUMENT still leaks the producer's +1:
//   that is #7188 ("a STATIC BUILTIN call whose argument is a +1 string
//   producer leaks that producer on the NORMAL path"), a separate defect in a
//   separate campaign.
// * A DISCARDED result (`Directory.GetFiles(d);` as a bare statement) still
//   leaks the result: that is #6421 ("a discarded _ := expression leaks
//   3,532 B / 201 allocs"). It is not specific to `Directory.*`.
// * `File.*` / `Path.*` and the `strToPath` copies are #8016's / #7865's
//   subjects; this file is about the runtime's `char**` return buffer alone.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
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

// THE ORACLE. `leakedAllocations` is the COUNT of allocations LeakSanitizer
// reports as leaked: 0 when there is no SUMMARY line (LSan prints one only
// when it finds leaks), and the summary's own number when there is one. The
// pre-fix compiler produced 300 / 200 / 100 here; the fixed compiler produces
// 0.
//
// `leakedBytes` is the same measurement in bytes.
//
// The COUNT is printed so a failing row's log carries the NUMBER the leak
// oracle measured.
void expectNoLeak(const JitResult& r, const std::string& out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// Splice the per-process data directory into a program that writes files
// (#5230): two test processes running the same suite concurrently must not
// race on a FIXED path.
std::string withData(const std::string& program) {
    return bezel::test::spliceDataPaths(program, "bzt8024");
}

// The shape every leak row uses: a directory that EXISTS and holds two
// regular files and one subdirectory. `Directory.GetFiles` on it yields the
// two files, `Directory.GetDirectories` yields the one subdirectory.
//
// The directory is created by the program itself, so every row is
// self-contained and the file census is EXACT (the per-process @DATA@
// directory is shared by every row in this binary, so the rows read their own
// subdirectory and never the shared one).
constexpr const char* kSetup = R"BZL(
Directory.CreateDirectory("@DATA@/bzt8024_d");
File.WriteAllText("@DATA@/bzt8024_d/f1.txt", "one");
File.WriteAllText("@DATA@/bzt8024_d/f2.txt", "two");
Directory.CreateDirectory("@DATA@/bzt8024_d/sub");
)BZL";

// Wrap a body fragment in a module + a class + a Main, with the setup
// fragment INSIDE Main (a fragment at the top level would not parse).
std::string program(const std::string& setup, const std::string& body) {
    return std::string("module M;\nclass P\npublic static method Main()\n") +
           setup + body + "end Main; end P;\n";
}

}  // namespace

// THE LEAK ROW: `Directory.GetFiles` over a directory that EXISTS and holds
// two files, 100 times.
//
// Pre-fix this read 300 allocations / 11600 B -- per call the runtime's
// `char**` array (1) plus its two `strdup`'d entries (2). The fixed compiler
// reads 0.
BEZEL_TEST(dir_get_files_buffer_leak_8024,
           directory_get_files_existing_dir_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(withData(program(kSetup, R"BZL(
var i := 0; while i < 100 do using var a := Directory.GetFiles("@DATA@/bzt8024_d"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// THE SAME SHAPE for the sibling member: `Directory.GetDirectories` over a
// directory that EXISTS and holds one subdirectory, 100 times.
//
// Pre-fix this read 200 allocations -- per call the `char**` array (1) plus
// its one `strdup`'d entry (1).
BEZEL_TEST(dir_get_files_buffer_leak_8024,
           directory_get_directories_existing_dir_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(withData(program(kSetup, R"BZL(
var i := 0; while i < 100 do using var a := Directory.GetDirectories("@DATA@/bzt8024_d"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// THE EMPTY-DIRECTORY EDGE: the directory EXISTS but holds nothing, so the
// runtime returns a `malloc(0)` array and NO entries. The array is still an
// allocation the caller must release.
//
// Pre-fix this read 100 allocations (one `malloc(0)` block per call). The
// fixed compiler reads 0 -- and it must not crash on the zero-length array.
BEZEL_TEST(dir_get_files_buffer_leak_8024,
           directory_get_files_empty_dir_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(withData(program(R"BZL(
Directory.CreateDirectory("@DATA@/bzt8024_empty");
)BZL", R"BZL(
var i := 0; while i < 100 do using var a := Directory.GetFiles("@DATA@/bzt8024_empty"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// THE MISSING-DIRECTORY CONTROL (GREEN on BOTH arms): for a directory that
// does not exist the runtime returns `nullptr` with a count of 0, so there is
// nothing to release and nothing to leak -- on the pre-fix arm OR on the fixed
// one. It is here to pin that the release path handles the runtime's null
// return without crashing (`free(nullptr)` is a no-op) and that the fix does
// not invent a leak on a path that had none.
BEZEL_TEST(dir_get_files_buffer_leak_8024,
           directory_get_files_missing_dir_is_clean_on_both_arms_spec_28_9) {
    expectNoLeak(runAotChecked(withData(program("", R"BZL(
var i := 0; while i < 100 do using var a := Directory.GetFiles("@DATA@/bzt8024_no_such_dir"); i := i + 1; end; Console.WriteLine("done");
)BZL"))),
                 "done\n");
}

// THE BEHAVIOUR CONTROL (GREEN on BOTH arms): the fix must not change WHAT
// `Directory.GetFiles` / `Directory.GetDirectories` compute. stdlib §7.4
// declares them as returning `owned Array<string>`; the runtime returns each
// entry's FULL path (the directory path joined with the entry's name), so the
// rows pin the COUNT and the membership, not the order (the runtime's order
// is the filesystem's `readdir` order, which the spec does not fix).
//
// This row runs on the plain `runAot` oracle: its subject is the VALUES, not
// the leak (the leak rows above carry the count). The leak-freedom of these
// very shapes is covered by the rows above.
BEZEL_TEST(dir_get_files_buffer_leak_8024,
           directory_get_files_result_is_correct_spec_7_4) {
    auto r = runAot(withData(program(kSetup, R"BZL(
using var files := Directory.GetFiles("@DATA@/bzt8024_d");
Console.WriteLine(files.Count);
if files.Contains("@DATA@/bzt8024_d/f1.txt") then Console.WriteLine(1); else Console.WriteLine(0); end;
if files.Contains("@DATA@/bzt8024_d/f2.txt") then Console.WriteLine(1); else Console.WriteLine(0); end;
if files.Contains("@DATA@/bzt8024_d/sub") then Console.WriteLine(0); else Console.WriteLine(1); end;
)BZL")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n1\n1\n1\n"));
}

// THE BEHAVIOUR CONTROL for the sibling member: `GetDirectories` yields the
// subdirectory and NOT the two regular files (stdlib §7.4 / the runtime's
// `S_ISDIR` filter).
BEZEL_TEST(dir_get_files_buffer_leak_8024,
           directory_get_directories_result_is_correct_spec_7_4) {
    auto r = runAot(withData(program(kSetup, R"BZL(
using var dirs := Directory.GetDirectories("@DATA@/bzt8024_d");
Console.WriteLine(dirs.Count);
if dirs.Contains("@DATA@/bzt8024_d/sub") then Console.WriteLine(1); else Console.WriteLine(0); end;
if dirs.Contains("@DATA@/bzt8024_d/f1.txt") then Console.WriteLine(0); else Console.WriteLine(1); end;
)BZL")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n1\n1\n"));
}
