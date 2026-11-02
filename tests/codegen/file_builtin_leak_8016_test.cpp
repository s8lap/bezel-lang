// Regression tests for #8016 -- the §7.3 `File.*` / §7.4 `Directory.*` arms
// leaked the `strToPath` C-string copy, and `File.ReadAllText` leaked its
// RESULT string too.
//
// MEASURED on the R38 staging tip `8993d643` (pre-fix, generated-code
// LeakSanitizer; the reviewer measured the same numbers):
//
//   `File.Exists("/no/such/file")` 100x        ->  99 allocations / 1386 B
//   `File.ReadAllText("/no/such/file")` 100x   -> 200 allocations / 4600 B
//
// TWO independent defects, in the SIBLING arms of the same function the
// #7865 fix changed (`tryLowerBuiltinClassCall`, LlvmBackend_Calls.cpp):
//
//   (A) THE C-STRING COPY. `strToPath` mallocs a NUL-terminated COPY of the
//       argument's bytes so the `bz_file_*` / `bz_dir_*` runtime entry point
//       can take a `const char*`. Every one of those entry points only READS
//       that pointer (`fopen`, `access`, `unlink`, `opendir`, `strdup`+copy)
//       and NONE of them takes ownership of it, so the copy is the CALLER's
//       to release -- and nobody released it: the malloc'd buffer was
//       unreachable the moment the call returned. The `Path` arms released
//       theirs (#7865); the `File` / `Directory` arms did not.
//
//   (B) THE RESULT. stdlib §7.3 declares `File.ReadAllText(path: string):
//       string` -- the ONLY §7.3 `File` member whose result is a `string`.
//       The `File` arm mints that result with `ptrToStr` -- a `calloc`'d
//       header at rc := 1 (`initStringRc`) whose data field is the
//       `bz_file_read_all_text` buffer -- so the CALLER owns exactly ONE
//       reference to it (§28.9: `string` is runtime-managed; "how the runtime
//       manages their lifecycle ... is not part of this specification"; "the
//       implementation reclaims the production"). But no consumer knew:
//       `isPlusOneStringProducer` did not recognise the §7.3 `File` family
//       (#7865's arm matches the keyword `Path` ONLY), so every consumer
//       treated the result as a BORROW and RETAINED on top of the producer's
//       +1 (rc 1 -> 2). The scope-end release then dropped only the
//       consumer's own share (rc 2 -> 1) and the producer's reference was
//       never discharged, so the 32-byte header AND its data buffer leaked
//       on every call.
//
// -- THE LEAK ORACLE (this is the point of the file) --------------------------
// A row that asserted only the program's OUTPUT would be GREEN on the pre-fix
// code and would prove nothing. Every row here measures a COUNT: the number of
// allocations LeakSanitizer reports as leaked, read out of LSan's own
// "SUMMARY: AddressSanitizer: <bytes> byte(s) leaked in <allocations>
// allocation(s)." line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO;
// on the pre-fix compiler the same rows read 99 / 100 / 199 / 200. The rows
// also run under `runAotChecked`, whose LSan leg turns a leak into a failed
// row, so the count and the verdict agree.
//
// Every row loops 100 times: a one-shot leak repro is a vacuous oracle (the
// issue's own one-shot `File.Exists` reports only ONE C-string copy -- the
// last one is still reachable at exit -- so a one-shot row would understate
// the count by an order of magnitude, and a one-shot row for the RESULT leak
// would miss it entirely, the header still being reachable at exit).
//
// -- THE BEHAVIOUR ROW -------------------------------------------------------
// The fix must not change what a `File` / `Directory` member computes:
// `File.Exists` still answers the same, `File.ReadAllText` still reads the
// same bytes, `File.ReadAllLines` still yields the same count. The behaviour
// row pins that, so a "fix" that silenced the leak by returning the wrong
// value would be RED.
//
// -- A USER-DECLARED `class File` IS NOT THE BUILTIN --------------------------
// `tryLowerBuiltinClassCall` keys its §7 arms on the receiver's IDENTIFIER
// TEXT, so a user class that merely spells `File` must not be lowered by the
// builtin arm -- and `isPlusOneStringProducer` must not classify the user's
// own `File.ReadAllText` as a builtin producer (it guards on
// `!isUserDeclaredTypeSym` and `!calleeResolvedBySema`, exactly as #7865's
// `Path` arm does). The last row pins both.
//
// -- WHAT IS NOT HERE, AND WHY -----------------------------------------------
// * A +1 string PRODUCER passed as an ARGUMENT (`File.ReadAllText(
//   String.Repeat("x", 3))`) still leaks the producer's +1: that is #7188
//   ("a STATIC BUILTIN call whose argument is a +1 string producer leaks that
//   producer on the NORMAL path"), a separate defect in a separate campaign.
//   The `Path` arm fixed its own half of it (#7865); this issue is about the
//   C-string copy and the RESULT.
// * A DISCARDED result (`File.ReadAllText("x");` as a bare statement) still
//   leaks the result: that is #6421 ("a discarded _ := expression leaks
//   3,532 B / 201 allocs"). The already-fixed `Path` family leaks the same
//   way (MEASURED: `Path.Combine("x","y");` as a statement leaks 200
//   allocations / 3600 B), so it is not a `File`-specific defect.
// * `Directory.GetFiles` on a directory that EXISTS still leaks the runtime's
//   `char**` return buffer (`bz_dir_get_files` mallocs the array and `strdup`s
//   each entry, and the codegen copies them out but never frees them). That is
//   a THIRD defect, filed separately; the row below therefore reads a
//   directory that does NOT exist, so the count it measures is exactly the
//   C-string copy this issue is about.

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
// pre-fix compiler produced 99 / 100 / 199 / 200 here; the fixed compiler
// produces 0.
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

// Splice the per-process data directory into a program that writes a file
// (#5230): two test processes running the same suite concurrently must not
// race on a FIXED path.
std::string withData(const std::string& program) {
    return bezel::test::spliceDataPaths(program, "bzt8016");
}

}  // namespace

// THE REVIEWER'S REPRO, verbatim: `File.Exists` over a missing path, 100
// times. Pre-fix this read 99 allocations / 1386 B -- one `strToPath` copy per
// call (the last one is still reachable at exit, so ASan reports 99 of 100).
BEZEL_TEST(file_builtin_leak_8016,
           file_exists_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do if File.Exists("/no/such/file") then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// THE RESULT LEAK: `File.ReadAllText` bound to a local, 100 times. Pre-fix
// this read 200 allocations / 4600 B -- 100 result headers (32 B) + 100
// result data buffers + the 100 `strToPath` C-string copies. Both halves are
// this issue: the copy is (A), the header and the payload are (B).
BEZEL_TEST(file_builtin_leak_8016,
           file_read_all_text_bound_local_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do var s := File.ReadAllText("/no/such/file"); i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// The RESULT consumed as a CALL ARGUMENT rather than bound to a local: the
// console write borrows it and the producer's +1 is discharged at the end of
// the full expression (`Console.Write`'s #1031 arm). Pre-fix this leaked the
// result header AND its payload on every iteration.
BEZEL_TEST(file_builtin_leak_8016,
           file_read_all_text_console_write_loop_100_spec_28_9) {
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += '\n';
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do Console.WriteLine(File.ReadAllText("/no/such/file")); i := i + 1; end;
end Main; end P;
)BZL"),
                 expected);
}

// THE RESULT WITH A REAL PAYLOAD: a file is written once and then read 100
// times, so the result is a fresh `bz.string` over a REAL byte buffer (not
// the null buffer of a missing file). Pre-fix this leaked the copy, the
// header, and the payload on every iteration.
BEZEL_TEST(file_builtin_leak_8016,
           file_read_all_text_round_trip_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(withData(R"BZL(
module M;

class P

public static method Main()
File.WriteAllText("@DATA@/bzt8016_rt.txt", "hello"); var i := 0; while i < 100 do var s := File.ReadAllText("@DATA@/bzt8016_rt.txt"); if s <> "hello" then Console.WriteLine("bad"); end; i := i + 1; end; File.Delete("@DATA@/bzt8016_rt.txt"); Console.WriteLine("done");
end Main; end P;
)BZL")),
                 "done\n");
}

// A §7.3 member whose result is NOT a string: the C-string copy is the only
// leak there. `File.WriteAllText` takes a path AND a contents string, so it
// makes exactly one `strToPath` copy.
BEZEL_TEST(file_builtin_leak_8016,
           file_write_all_text_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(withData(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do File.WriteAllText("@DATA@/bzt8016_w.txt", "x"); i := i + 1; end; File.Delete("@DATA@/bzt8016_w.txt"); Console.WriteLine("done");
end Main; end P;
)BZL")),
                 "done\n");
}

// `File.ReadAllLines` returns an `owned Array<string>`, not a `string`: the
// C-string copy is its only leak.
BEZEL_TEST(file_builtin_leak_8016,
           file_read_all_lines_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do using var a := File.ReadAllLines("/no/such/file"); i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// `File.ReadAllBytes` returns an `owned Array<byte>`: the C-string copy is its
// only leak.
BEZEL_TEST(file_builtin_leak_8016,
           file_read_all_bytes_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do using var a := File.ReadAllBytes("/no/such/file"); i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// `File.Copy` takes TWO paths, so it makes TWO `strToPath` copies -- the row
// pins that BOTH are released. Pre-fix this read 199 allocations / 2189 B.
BEZEL_TEST(file_builtin_leak_8016,
           file_copy_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do File.Copy("/no/such/a", "/no/such/b", false); i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// `File.Move` also takes two paths.
BEZEL_TEST(file_builtin_leak_8016,
           file_move_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do File.Move("/no/such/a", "/no/such/b"); i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// `File.Delete` takes one path.
BEZEL_TEST(file_builtin_leak_8016,
           file_delete_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do File.Delete("/no/such/file"); i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// THE §7.4 `Directory` FAMILY. `Directory.Exists` is the `Directory` twin of
// `File.Exists`: one `strToPath` copy per call, no `string` result.
BEZEL_TEST(file_builtin_leak_8016,
           directory_exists_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do if Directory.Exists("/no/such/dir") then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// `Directory.GetFiles` returns an `owned Array<string>`. The directory does
// NOT exist here on purpose: the runtime's `char**` return buffer is a
// SEPARATE defect (see the file header), and a directory that EXISTS would
// add that leak to the count this row measures.
BEZEL_TEST(file_builtin_leak_8016,
           directory_get_files_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do using var a := Directory.GetFiles("/no/such/dir"); i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// `Directory.CreateDirectory` + `Directory.Delete` — TWO §7.4 arms, TWO
// `strToPath` copies per iteration.
BEZEL_TEST(file_builtin_leak_8016,
           directory_create_delete_loop_100_spec_28_9) {
    expectNoLeak(runAotChecked(withData(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do Directory.CreateDirectory("@DATA@/bzt8016_d"); Directory.Delete("@DATA@/bzt8016_d"); i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL")),
                 "done\n");
}

// A USER-DECLARED `class File` IS NOT THE BUILTIN. The user's own
// `ReadAllText` must run (the values say so), and the builtin's
// `isPlusOneStringProducer` arm must not classify it (the count says so).
// This is the `Path` row #7865 shipped, spelled for `File`.
BEZEL_TEST(file_builtin_leak_8016,
           user_declared_file_class_is_not_the_builtin_spec_28_9) {
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "u\n";
    expected += "done\n";
    expectNoLeak(runAotChecked(R"BZL(
module M;

class File
  public static method ReadAllText(path: string): string
    return "USER" + path;
  end ReadAllText;
  public static method Exists(path: string): bool
    return path = "USER";
  end Exists;
end File;

class P

public static method Main()
var i := 0; while i < 100 do var r := File.ReadAllText("x"); if r <> "USERx" then Console.WriteLine("bad"); end; if File.Exists("USER") then Console.WriteLine("u"); else Console.WriteLine("n"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 expected);
}

// THE BEHAVIOUR ROW (stdlib §7.3 / §7.4). The leak fix must not change what
// a `File` / `Directory` member computes: `File.Exists` still answers the
// same, `File.ReadAllText` still reads the same bytes, `File.ReadAllLines`
// still yields the same count. A "fix" that silenced the leak by returning
// the wrong value would be RED here.
//
// This row runs on the plain `runAot` oracle: its subject is the VALUES, not
// the leak (the leak rows above carry the count). `runAotChecked` would also
// work, but its sanitizer report for this many distinct allocation sites is
// long enough to be truncated by the harness's 8 KiB diagnostic cap, which
// would leave the COUNT unparseable here -- and a count that cannot be
// parsed is not an oracle. The leak-freedom of these very shapes is covered
// by the rows above.
BEZEL_TEST(file_builtin_leak_8016,
           file_and_directory_members_still_behave_spec_7_3) {
    auto r = runAot(R"BZL(
module M;

class P

public static method Main()
Console.WriteLine(File.Exists("/no/such/file"));
Console.WriteLine(Directory.Exists("/"));
Console.WriteLine("[" + File.ReadAllText("/no/such/file") + "]");
using var lines := File.ReadAllLines("/no/such/file");
Console.WriteLine(lines.Count);
using var bytes := File.ReadAllBytes("/no/such/file");
Console.WriteLine(bytes.Count);
end Main; end P;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nTrue\n[]\n0\n0\n"));
}
