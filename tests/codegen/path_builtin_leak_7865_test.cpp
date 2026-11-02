// Regression tests for #7865 -- EVERY `Path.*` call leaked.
//
// The issue's own repro: `Path.Combine("x","y")` leaks 2 allocations -- the
// malloc'd C-string COPIES `strToPath` makes of the arguments (one per
// argument). MEASURED by the R37 rev-c2-39 review on r30-c1; reproduced and
// measured here on the R38 staging tip `6e087324` (pre-fix, under
// generated-code LeakSanitizer):
//
//   `Console.WriteLine(Path.Combine("x","y"))`            -> 2 allocations
//   100 x `Path.Combine(String.Repeat("x",3),
//                         String.Repeat("y",3))`           -> 799 allocations
//   100 x `Path.Combine(b := b, a := a)`  (borrowed)      -> 402 allocations
//
// THREE allocations leaked per `Path.Combine` call, and each is a DIFFERENT
// ownership defect:
//
//   (A) THE C-STRING COPY. `strToPath` (LlvmBackend_Calls.cpp) mallocs a
//       NUL-terminated COPY of the argument's bytes so the `bz_path_*` runtime
//       entry point can take a `const char*`. Every `bz_path_*` entry point
//       COPIES its input into a fresh buffer (`bz_path_combine` memcpy's, the
//       rest `strdup` / `malloc`+copy), so the copy is the CALLER's to
//       release -- and nobody released it. The issue's "leaks 2 allocations"
//       is exactly these two copies.
//
//   (B) THE RESULT. stdlib §7.4 declares `Path` a `public static class` whose
//       six members all return `string`; there is NO `Path` value type. The
//       `Path` arm of `tryLowerBuiltinClassCall` mints that result with
//       `ptrToStr` -- a `calloc`'d header at rc := 1 (`initStringRc`) whose
//       data field is the `bz_path_*` buffer -- so the CALLER owns exactly ONE
//       reference to it (§28.3's producers arm: "Fresh productions are
//       owned"; §4.6: `string` is runtime-managed; §28.9: the reference
//       count). But no consumer knew that: `isPlusOneStringProducer` did not
//       recognise the `Path` family, so every consumer treated the result as
//       a BORROW and RETAINED on top of the producer's +1 (rc 1 -> 2). The
//       scope-end release then dropped only the consumer's own share
//       (rc 2 -> 1) and the producer's reference was never discharged, so the
//       32-byte header AND its data buffer leaked on every call. The SAME gap
//       as #2228 (`string.From` / `string.Repeat`) and #7464
//       (`Convert.ToString`).
//
//   (C) THE ARGUMENT TEMPORARY. §28.3: a non-`owned` argument position is a
//       BORROWING use whatever the callee's body does with the parameter, so
//       an argument that is itself a +1 producer (`String.Repeat("x", 3)`) is
//       still an unbound owned temporary, disposed at the end of the full
//       expression (§28.3 K8.1: "Unbound owned temporaries are disposed at
//       their full expression's end, after their uses, in reverse production
//       order"). The general call path discharges it (the post-call
//       `emitStringRelease` per producer argument) and `Console.Write`'s
//       #1031 arm discharges it; the `Path` arm did NEITHER, so the
//       argument's +1 survived the call.
//
// ── THE LEAK ORACLE (this is the point of the file) ────────────────────────
// A row that asserted only the program's OUTPUT would be GREEN on the pre-fix
// code and would prove nothing. Every row here measures a COUNT: the number of
// allocations LeakSanitizer reports as leaked, read out of LSan's own
// "SUMMARY: AddressSanitizer: <bytes> byte(s) leaked in <allocations>
// allocation(s)." line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO;
// on the pre-fix compiler the same rows read 799 / 402 / 400. The rows also
// run under `runAotChecked`, whose LSan leg turns a leak into a failed row,
// so the count and the verdict agree.
//
// Every row loops 100 times: a one-shot leak repro is a vacuous oracle (the
// issue's own one-shot `Path.Combine("x","y")` reports only the two C-string
// copies -- its result header is still reachable at exit -- so a one-shot row
// would MISS the result leak entirely).
//
// ── THE BEHAVIOUR ROW ──────────────────────────────────────────────────────
// The fix must not change what a `Path` member computes: `Combine` still
// concatenates with a single '/', the five one-argument members still return
// the same components, and the results still compare equal. The last row
// pins that, so a "fix" that silenced the leak by returning the wrong value
// would be RED.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
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
// pre-fix compiler produced 799 / 402 / 400 here; the fixed compiler produces
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

}  // namespace

// THE ISSUE'S POSITIONAL SHAPE, looped so the count is not vacuous:
// `Path.Combine("x","y")` -- two arguments, both literals. Pre-fix this read
// 400 allocations over 100 iterations (200 C-string copies + 100 result
// headers + 100 result payloads).
BEZEL_TEST(path_builtin_leak_7865,
           path_combine_positional_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do var r := Path.Combine("x", "y"); if r.Length <> 3 then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// THE ISSUE'S LOOP/PRODUCER SHAPE, verbatim: the two ARGUMENTS are +1 string
// producers (`String.Repeat`), so each is an unbound owned temporary that
// §28.3 K8.1 disposes at the full expression's end. Pre-fix this read 799
// allocations, and the LSan breakdown names all THREE families:
//
//   200 x 32 B   the ARGUMENT string headers (100 iterations x 2 `String.Repeat`)
//   200 x  4 B   the ARGUMENT string payloads   (the "xxx" / "yyy" data buffers)
//   199 x  4 B   the `strToPath` C-string copies (the two per call)
//   100 x 32 B   the RESULT string headers
//   100 x  8 B   the RESULT string payloads     (the "xxx/yyy" data buffer)
//
// = 799. After the fix every one of them is discharged.
BEZEL_TEST(path_builtin_leak_7865,
           path_combine_producer_arguments_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do var r := Path.Combine(String.Repeat("x", 3), String.Repeat("y", 3)); if r.Length <> 7 then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// THE ISSUE'S BORROWED-LOCAL SHAPE, verbatim: the arguments are NAMED LOCALS
// (borrows, not producers), so nothing about them may be released -- a
// release here would drop the owner's only reference and ASan would report a
// use-after-free. Pre-fix this read 402 allocations even though every
// argument was a borrow: the C-string copies and the result still leaked.
BEZEL_TEST(path_builtin_leak_7865,
           path_combine_borrowed_locals_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var a := "A"; var b := "B"; var i := 0; while i < 100 do var r := Path.Combine(b := b, a := a); if r.Length <> 3 then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine(Path.Combine(b := b, a := a));
end Main; end P;
)BZL"),
                 "A/B\n");
}

// A ONE-ARGUMENT member: `GetFileName` mints its result the same way
// (`ptrToStr`), and its argument's C-string copy leaked the same way. Pre-fix
// this read 200 allocations over 100 iterations (100 C-string copies + 100
// result headers).
BEZEL_TEST(path_builtin_leak_7865,
           path_get_file_name_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do var r := Path.GetFileName("/a/b/c.txt"); if r <> "c.txt" then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// The result consumed as a CALL ARGUMENT rather than bound to a local: the
// console write borrows it and the producer's +1 is discharged at the end of
// the full expression. Pre-fix this leaked the result header on every
// iteration (the one-shot spelling of it is NOT reported at all -- the
// header is still reachable at exit -- so the row has to loop).
BEZEL_TEST(path_builtin_leak_7865,
           path_combine_console_write_loop_100_spec_28_3) {
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "x/y\n";
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do Console.WriteLine(Path.Combine("x", "y")); i := i + 1; end;
end Main; end P;
)BZL"),
                 expected);
}

// THE RESULT RETURNED BY A METHOD. The `Path` result is a +1 the CALLEE
// transfers to its caller (§28.3's producers arm + §28.9's mint rule), so the
// caller owns it and the callee must NOT release it. Pre-fix this leaked the
// result header and payload on every iteration.
BEZEL_TEST(path_builtin_leak_7865,
           path_result_returned_from_method_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class Q
  public static method Make(): string
    return Path.Combine("m", "n");
  end Make;
end Q;

class P

public static method Main()
var i := 0; while i < 100 do var r := Q.Make(); if r <> "m/n" then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// THE RESULT OVERWRITTEN IN ITS SLOT. A local that is REASSIGNED releases the
// value it held before the overwrite (§28.9): the producer's +1 is discharged
// exactly once, and the new value's +1 takes the slot. A fix that released
// twice here would show up as a double free (ASan) -- a leak-oracle row is
// RED on that, which is why this row asserts the count and not merely "no
// crash".
BEZEL_TEST(path_builtin_leak_7865,
           path_result_reassigned_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do var s := Path.Combine("s", "t"); s := Path.Combine("u", "v"); if s <> "u/v" then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// THE RESULT CONCATENATED. The `+` operator's operand handling: a +1
// producer operand is TRANSFERRED into the concatenation and the producer's
// reference is discharged; a borrow operand is retained. This row pins that
// the `Path` result is on the producer side (the concatenation's own result
// is the only thing the row's slot owns).
BEZEL_TEST(path_builtin_leak_7865,
           path_result_in_concatenation_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M;

class P

public static method Main()
var i := 0; while i < 100 do var s := "z" + Path.Combine("j", "k"); if s <> "zj/k" then Console.WriteLine("bad"); end; i := i + 1; end; Console.WriteLine("done");
end Main; end P;
)BZL"),
                 "done\n");
}

// THE BEHAVIOUR ROW (stdlib §7.4). The leak fix must not change what a
// `Path` member computes: `Combine` still concatenates with a single '/', the
// five one-argument members still return the same components, and the results
// still compare equal. A "fix" that silenced the leak by returning the wrong
// value would be RED here.
//
// This row runs on the plain `runAot` oracle: its subject is the VALUES, not
// the leak (the leak rows above carry the count). `runAotChecked` would also
// work, but its sanitizer report for this many distinct allocation sites is
// long enough to be truncated by the harness's 8 KiB diagnostic cap, which
// would leave the COUNT unparseable here -- and a count that cannot be
// parsed is not an oracle. The leak-freedom of these very shapes is covered
// by the rows above.
BEZEL_TEST(path_builtin_leak_7865,
           path_members_still_behave_spec_7_4) {
    auto r = runAot(R"BZL(
module M;

class Q
  public static method Take(s: string): string
    return "[" + s + "]";
  end Take;
  public static method Make(): string
    return Path.Combine("m", "n");
  end Make;
end Q;

class P

public static method Main()
Console.WriteLine(Path.Combine("x", "y"));
Console.WriteLine(Path.GetFileName("/a/b/c.txt"));
Console.WriteLine(Path.GetExtension("/a/b/c.txt"));
Console.WriteLine(Path.GetFileNameWithoutExtension("/a/b/c.txt"));
Console.WriteLine(Path.GetDirectoryName("/a/b/c.txt"));
Console.WriteLine(Path.Combine("a", "b") = "a/b");
Console.WriteLine(Path.GetFileName("/a/b/c.txt") = "c.txt");
Console.WriteLine(Q.Take(Path.Combine("q", "r")));
Console.WriteLine(Q.Make());
var s := Path.Combine("s", "t");
Console.WriteLine(s);
s := Path.Combine("u", "v");
Console.WriteLine(s);
Console.WriteLine("z" + Path.Combine("j", "k"));
end Main; end P;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("x/y\nc.txt\n.txt\nc\n/a/b\nTrue\nTrue\n[q/r]\n"
                                "m/n\ns/t\nu/v\nzj/k\n"));
}
