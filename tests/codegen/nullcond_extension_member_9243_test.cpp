// fix #9243 (§7.5, §26.5 rule 2, §26.1, v0.359) — the `?.` spelling of an
// EXTENSION member over a VALUE-shaped receiver PERFORMS the access.
//
// THE DEFECT. §7.5 (v0.359): "`x?.M` returns null if `x` is null; otherwise it
// performs the access", and §26.5 rule 2 (v0.359): "The outcome is the same
// for every member access on a value, not only a call … with a `?.` or
// postfix-`!` receiver read at its underlying non-nullable type (§7.5, §7.8)"
// — so the `?.` spelling of a member resolves it EXACTLY as the `.` spelling
// does.
//
// But codegen's null-conditional CALL lowering (`lowerNullCondCall`,
// src/codegen/LlvmBackend_Calls2.cpp) has a "fail-fast on non-pointer (old
// guard)" that DROPPED the call whenever the receiver's lowered value was not
// an object POINTER — and a VALUE-shaped receiver is not a pointer: a
// primitive, an enum, a built-in value type, or the console stream's i32
// stream id (stdlib §2.1 / §7.4). The call then lowered to NOTHING, the
// enclosing store degraded to a zero value behind codegen's #2434 backstop
// (`internal: lowerLocalDecl's initializer expression failed to lower to a
// value`), and the program exited 0 having printed NOTHING — while the `.`
// spelling of the SAME member on the SAME receiver performed it.
//
// MEASURED ON THE BASE (r30-c1 @ d66898662, `bezelc` rebuilt, ASan leg):
//
//   over `extension int`        — `x.F()` printed `1`; `x?.F()` printed
//                                  NOTHING (and emitted the #2434 degrade)
//   over `extension DateTime`   — `d.F()` printed `1`; `d?.F()` printed
//                                  NOTHING (and emitted the #2434 degrade)
//   over `extension TextWriter` — `w.Bogus()` printed `7`; `w?.Bogus()`
//                                  printed NOTHING (and emitted the #2434
//                                  degrade)
//
// and over `extension TextReader` — the ISSUE'S OWN PROGRAM — `r.Bogus()`
// printed `7` while `r?.Bogus()` printed NOTHING. So the two access forms of
// ONE member disagreed, in the `?.`-family's silent-wrong-behaviour shape
// (the program exits 0 having printed nothing).
//
// The `.` spelling has NO such guard: it shapes the receiver to the callee's
// `this` and passes it (the §26.1 block in `lowerCallDispatch`, through
// `extensionThisType`), so the two spellings disagreed about the SAME member.
//
// THE FIX. The receiver is admitted when the CALLEE's own `this` IS that
// value — read through `extensionThisType` (§26.1), the SAME helper the `.`
// spelling's receiver shaping uses, so the value crosses the call exactly as
// the `.` spelling passes it. A class / interface / record target takes its
// `this` by POINTER (`byPointer`), so it is not this fix's population and is
// left alone.
//
// The NULL arm. §7.5's short-circuit tests the receiver for null. A
// NON-nullable VALUE receiver admits no null, so the null arm is DEAD: the
// access always performs, exactly as the `.` spelling's does. The branch is
// still emitted (with a constant-false condition) so the result PHI keeps its
// two well-formed CFG edges; the RESULT is still lifted to the
// always-nullable form (§7.5: "The result type is always nullable").
//
// WHAT IS NOT TOUCHED. A NULLABLE value receiver (`int?`, `DateTime?`) was
// already lowered by the nullable-value arm (the `{i8 hasValue; T}` carrier)
// and is unchanged. A POINTER receiver (a user class, a stdlib class) was
// already lowered and is unchanged. A class/interface/record extension target
// keeps its `this`-by-pointer shaping.
//
// ORACLE. `runAot` — the strict AOT path (fork-isolated Driver::compileFile +
// clang link + native exec), never the deprecated in-process JIT harness. The
// rows assert the program's own stdout and exit code, and the ASan /
// LeakSanitizer legs: `BEZEL_AOT_FORCE_SANITIZE=address` makes the harness
// instrument the GENERATED module, and the row fails if the sanitizer leg
// reported anything.
//
// FAIL-WITHOUT / PASS-WITH. Every row is RED on the base (r30-c1 @ d66898662):
// the program compiles, runs, exits 0 and prints NOTHING, so the row fails on
// `stdoutText` (the expected `7` / `1` is missing) — a silent wrong value,
// not a crash. Each row is GREEN with the fix.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

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

// The program ran to completion: it compiled, it exited normally with the
// program's own exit code and the EXACT expected output, and neither
// AddressSanitizer nor the runtime's §28.9 over-release check fired.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    didNotCrash(r, exitCode, out);
    assertNoLeak(r);
}

// The ISSUE'S OWN PROGRAM (issue #9243's "Repro" block, verbatim): an
// `extension TextReader` declaring `Bogus`, a `TextReader`-TYPED local, and
// the `?.` spelling of the extension member. §26.5 rule 2 resolves the
// extension member against the receiver's compile-time type, and §7.5 makes
// the `?.` spelling perform the access.
std::string textReaderProgram() {
    return
        "module M;\n"
        "import System.IO;\n"
        "public extension TextReader\n"
        "  public method Bogus(): int => 7;\n"
        "end;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var r: TextReader := Console.In;\n"
        "    var v := r?.Bogus();\n"
        "    if v <> null then\n"
        "      Console.WriteLine(v.ToString());\n"
        "    end if;\n"
        "  end Main;\n"
        "end Program;\n";
}

// The console stream's OTHER half: `TextWriter` (stdlib §2.1 / §7.4). Its
// value is the i32 stream descriptor, so it is the same VALUE-shaped
// receiver as the reader above — and the same rule.
std::string textWriterProgram() {
    return
        "module M;\n"
        "import System.IO;\n"
        "public extension TextWriter\n"
        "  public method Bogus(): int => 7;\n"
        "end;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var w: TextWriter := Console.Out;\n"
        "    var v := w?.Bogus();\n"
        "    if v <> null then\n"
        "      Console.WriteLine(v.ToString());\n"
        "    end if;\n"
        "  end Main;\n"
        "end Program;\n";
}

// The value-shaped receiver in its plainest form: an `extension int` over a
// NON-nullable `int`. The nullable twin (`int?`) is lowered by the
// nullable-value arm and is not this fix's population.
std::string intValueProgram() {
    return
        "module M;\n"
        "public extension int\n"
        "  public method F(): int => 1;\n"
        "end;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var x: int := 5;\n"
        "    var v := x?.F();\n"
        "    if v <> null then\n"
        "      Console.WriteLine(v.ToString());\n"
        "    end if;\n"
        "  end Main;\n"
        "end Program;\n";
}

}  // namespace

// §7.5 / §26.5 rule 2: the ISSUE'S OWN PROGRAM — the `?.` spelling of an
// extension member over a `TextReader`-typed receiver performs the access and
// prints the extension's own value.
BEZEL_TEST(nullcond_extension_member_9243,
           spec_7_5_extension_member_over_textreader_receiver_performs) {
    auto r = runAot(textReaderProgram());
    expectClean(r, 0, "7\n");
}

// §7.5 / §26.5 rule 2: the same rule over the console stream's other half —
// a `TextWriter`-typed receiver.
BEZEL_TEST(nullcond_extension_member_9243,
           spec_7_5_extension_member_over_textwriter_receiver_performs) {
    auto r = runAot(textWriterProgram());
    expectClean(r, 0, "7\n");
}

// §7.5 / §26.5 rule 2: the same rule over a plain value receiver — an
// `extension int` over a non-nullable `int`.
BEZEL_TEST(nullcond_extension_member_9243,
           spec_7_5_extension_member_over_int_value_receiver_performs) {
    auto r = runAot(intValueProgram());
    expectClean(r, 0, "1\n");
}
