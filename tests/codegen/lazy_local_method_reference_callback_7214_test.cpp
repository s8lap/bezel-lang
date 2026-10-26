// fix #7214 (§27.6 / stdlib §5, v0.344) — a §5 lazy operator whose CALLBACK
// is a METHOD REFERENCE HELD IN A LOCAL VARIABLE (`var f := Cat;
// xs.Zip(ys, f)`) was mis-lowered: the §5 lowering sizes its type-erased
// element slot AND types its trampoline's call from the callback's RESULT
// TYPE, and the local-held spelling carries NO Function TypeRef to read —
// sema types the QUALIFIED spelling (`Zip(ys, C.Cat)`) and the LAMBDA, but
// the BARE reference (`Cat`) that the local binds is left untyped (issue
// #7182: fix #4897, v0.307, synthesized the missing type only inside
// `checkAssignable`, so it never reached the expression — nor the INFERRED
// local that binds it, whose `declType` is null for the same reason).
//
// With no result type to read, the lowering fell back to `i32`, so the
// trampoline called the callback with an `i32` RESULT type and TRUNCATED the
// returned `string` pointer to 32 bits. The consumer then read the element
// out of the type-erased slot and dereferenced the mangled pointer.
//
// MEASURED (base `a9a512ff`, spec v0.344): the generated program SEGVs under
// AddressSanitizer —
//     ERROR: AddressSanitizer: SEGV on unknown address 0x0000227e0048
//     READ memory access, in main
// The plain (non-ASan) build happens to survive it, because the heap address
// of the element happens to fit in 32 bits on this target — the truncation is
// undefined behaviour, not a benign narrowing, so the ASan build is the
// oracle that sees it.
//
// The spelling is ADMITTED BY THE SPEC: stdlib §5's
// `Zip<U,R>(other, combine: (T,U) -> R)` takes a function VALUE, and §27.6 is
// the section that forms one — so a method reference is a legal combiner and
// the program is well-formed. The COMPILER was wrong to mis-lower it; the fix
// does NOT reject the shape. §22.5 / §22.6 govern a call that matches NO
// declared form, and this call matches stdlib §5's declared form.
//
// THE ORACLE. The rows below run under `runAotCheckedLenient` — the ASan
// oracle with the LEAK net off — because the local-held closure CELL leaks
// (64 B) for a reason of its own: the local's cell is not released when the
// local is passed as an argument (the same family as #5967 / #7201's
// residual), which is a SEPARATE defect with its own tracker and is NOT
// closed by this change. The row's subject is the CRASH: `runAotCheckedLenient`
// still instruments the generated code with AddressSanitizer and still fails
// the row when the program dies by a signal, so the SEGV is caught. The
// leak is deliberately not asserted here — pinning it as an expectation would
// document a defect as a requirement.
//
// Fail-without / passes-with: on the base (the staging tip `a9a512ff`) the
// programs below COMPILE CLEAN and then SEGV under ASan; with the fix the
// same programs run to completion and return the expected value.
//
// Spec: language spec §27.6 (*Method references*), §22.5 / §22.6 (a call
// matching no declared form), stdlib §5 (`System.Collections.Linq` — the
// deferred operators and their callback types), v0.344.

#include <cstdio>
#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAotCheckedLenient — the ASan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The outcome of one program under the ASan-armed AOT oracle.
//
// The MEMORY-SAFETY property is asserted DIRECTLY (`ran`), not through the
// exit code: a SEGV can land in mapped memory and exit 0 on some targets,
// so a row that only checked the exit code would pass on the base.
struct Checked {
    bool ran = false;             // the program ran to completion
    int exitCode = -1;            // its exit status (8-bit truncated)
    bool rejected = false;        // the compiler rejected the program
    bool sanitizerFired = false;  // the sanitizer net reported (SEGV / leak / UAF)
    std::string report;           // the sanitizer report, for the failure log
};

Checked runCheckedLenient(const std::string& src) {
    Checked c;
    auto r = runAotCheckedLenient(src);
    c.ran = r.ran;
    c.exitCode = r.exitCode;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) {
            c.sanitizerFired = true;
            c.report = d.message;
            continue;
        }
        // A compile-time rejection is reported as an error diagnostic too; it
        // is not a sanitizer finding, but the program never ran.
        c.rejected = true;
    }
    return c;
}

Checked runChecked(const std::string& src) {
    Checked c;
    auto r = runAotChecked(src);
    c.ran = r.ran;
    c.exitCode = r.exitCode;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) {
            c.sanitizerFired = true;
            c.report = d.message;
            continue;
        }
        c.rejected = true;
    }
    return c;
}

// The row's verdict: the program compiled, RAN TO COMPLETION (no SEGV), and
// returned the expected value.
//
// There is deliberately NO skip-absorbing guard here: a toolchain that cannot
// build the ASan executable must fail the row, never pass it.
void expectRuns(const std::string& src, int expected) {
    auto c = runCheckedLenient(src);
    if (c.rejected) {
        std::fprintf(stderr, "[7214] the program was REJECTED at compile time\n");
    }
    if (c.sanitizerFired) {
        std::fprintf(stderr, "[7214] sanitizer report:\n%s\n", c.report.c_str());
    }
    BEZEL_EXPECT_FALSE(c.rejected);
    BEZEL_EXPECT_FALSE(c.sanitizerFired);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, expected);
}

}  // namespace

// ===========================================================================
// THE PIN — #7214: a `Zip` whose combiner is a METHOD REFERENCE HELD IN A
// LOCAL. Before the fix: compiled clean, then an ASan SEGV in the generated
// program (the truncated element pointer). After the fix: it runs and returns
// the combined length.
// ===========================================================================
// The issue's own repro shape: `var f := Cat;` then `xs.Zip(ys, f)`.
BEZEL_TEST(lazy_local_method_reference_callback_7214,
           zip_local_method_reference_combiner) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method Cat(a: string, b: string): string => a + b; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    using var ys: Array<string> := [\"c\", \"dd\"]; "
        "    var f := Cat; "
        "    var total: int := 0; "
        "    for var w in xs.Zip(ys, f) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectRuns(src, 6);  // "ac".Length + "bbdd".Length == 2 + 4
}

// The same shape through `Select` — the §5 projection, whose element slot is
// sized from the projection's result type the same way.
BEZEL_TEST(lazy_local_method_reference_callback_7214,
           select_local_method_reference_projection) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method Make(x: string): string => x + \"!\"; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    var f := Make; "
        "    var total: int := 0; "
        "    for var w in xs.Select(f) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectRuns(src, 5);  // "a!".Length + "bb!".Length == 2 + 3
}

// ===========================================================================
// CONTROLS — the neighbouring spellings must keep behaving EXACTLY as they
// did before the fix. A change that moved one of these is a behaviour flip.
// ===========================================================================
// The ANNOTATED local — `var f: (string, string) -> string := Cat;` — carries
// the Function TypeRef on the local itself, so it was lowered correctly
// before the fix and must still be.
BEZEL_TEST(lazy_local_method_reference_callback_7214,
           zip_annotated_local_method_reference_combiner_unchanged) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method Cat(a: string, b: string): string => a; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    using var ys: Array<string> := [\"c\", \"dd\"]; "
        "    var f: (string, string) -> string := Cat; "
        "    var total: int := 0; "
        "    for var w in xs.Zip(ys, f) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectRuns(src, 3);  // "a".Length + "bb".Length == 1 + 2
}

// The BARE spelling passed straight to the operator (`xs.Zip(ys, Cat)`) —
// the one the §5 lowering resolves through the method reference itself. It
// was accepted and correct before the fix, and the enumerator OWNS its
// closure cell (so this program is LSan-clean): it is the strict-oracle
// control that shows the fix did not change the bare spelling.
BEZEL_TEST(lazy_local_method_reference_callback_7214,
           zip_bare_method_reference_combiner_unchanged) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method Cat(a: string, b: string): string => a; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    using var ys: Array<string> := [\"c\", \"dd\"]; "
        "    var total: int := 0; "
        "    for var w in xs.Zip(ys, Cat) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    auto c = runChecked(src);
    if (c.rejected) {
        std::fprintf(stderr, "[7214] the program was REJECTED at compile time\n");
    }
    if (c.sanitizerFired) {
        std::fprintf(stderr, "[7214] sanitizer report:\n%s\n", c.report.c_str());
    }
    BEZEL_EXPECT_FALSE(c.rejected);
    BEZEL_EXPECT_FALSE(c.sanitizerFired);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 3);
}
