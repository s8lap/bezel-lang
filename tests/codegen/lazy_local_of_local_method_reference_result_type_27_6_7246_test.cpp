// fix #7246 (§27.6 / stdlib §5, v0.344) — a §5 lazy operator whose CALLBACK
// is a METHOD REFERENCE held in a local that ITSELF holds a local (an ALIAS
// chain) was still mis-lowered: the §5 lowering sizes its type-erased
// element slot AND types its trampoline's call from the callback's RESULT
// TYPE, and the alias spelling carries NO Function TypeRef to read.
//
//     var f := CatMint;
//     var g := f;              // <-- the initializer is an IDENTIFIER
//     xs.Zip(ys, g);           // <-- the combiner's result type is unknown
//
// fix #7214 (v0.344) recovered that type from the local's INITIALIZER, so
// it worked ONE level deep (`var f := CatMint; xs.Zip(ys, f);`). At TWO
// levels the initializer is an identifier, which carries no type of its own,
// so no recovery happened and the lowering fell back to its `i32` default:
// the trampoline called the callback with an `i32` RESULT type and
// TRUNCATED the returned `string` pointer to 32 bits. The consumer then
// read the element out of the type-erased slot and dereferenced the mangled
// pointer.
//
// MEASURED (base `30849759`, spec v0.344) under AddressSanitizer:
//
//     ERROR: AddressSanitizer: SEGV on unknown address 0x000000008d5e0048
//     The signal is caused by a READ memory access.
//     r13 = 0x00007c608d5e0020     <- the real (ASan-heap) string pointer
//
// The reported address is exactly the LOW 32 BITS of that pointer — the
// truncation. Under a plain (non-ASan) build the heap happens to live below
// 4 GiB and the truncation round-trips by luck, so the plain build is NOT a
// discriminating oracle; the ASan build is (the allocator places objects
// high). The truncation is undefined behaviour either way.
//
// The spelling is ADMITTED BY THE SPEC: §27.6 reads a method reference's
// "formed type" as "the callee's own signature read as a function type", so
// the reference IS a function value, and stdlib §5's
// `Zip<U,R>(other: IEnumerable<U>, combine: (T, U) -> R)` takes a function
// value — so a method reference bound to a local, and then ALIASED through a
// second local, is an ordinary use of the same value (§12.7's
// *Function-typed targets* reads the alias `var g := f;` as an ordinary
// binding). The COMPILER was wrong to mis-lower it; the fix does NOT reject
// the shape.
//
// THE ORACLE. The rows below run under `runAotCheckedLenient` — the ASan
// oracle with the LEAK net off — for the reason fix #7214's sibling file
// records: the local-held closure CELL leaks (64 B) for a reason of its own
// (the local's cell is not released when the local is passed as an
// argument — the #5967 / #7201 residual family), and the leak report for it
// is NOT deterministic (LSan's conservative stack scan finds a stale pointer
// to the cell roughly half the time), so no row may assert it. The row's
// subject is the CRASH: `runAotCheckedLenient` still instruments the
// generated code with AddressSanitizer and still fails the row when the
// program dies by a signal, so the SEGV is caught. Pinning the leak as an
// expectation would document a defect as a requirement.
//
// Fail-without / passes-with: on the base the programs below COMPILE CLEAN
// and then SEGV under ASan; with the fix the same programs run to
// completion and return the expected value.
//
// Spec: language spec §27.6 (*Method references*), stdlib §5
// (`System.Collections.Linq` — the deferred operators and their callback
// types; `Zip<U,R>(other: IEnumerable<U>, combine: (T, U) -> R)`), v0.344.

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
        std::fprintf(stderr, "[7246] the program was REJECTED at compile time\n");
    }
    if (c.sanitizerFired) {
        std::fprintf(stderr, "[7246] sanitizer report:\n%s\n", c.report.c_str());
    }
    BEZEL_EXPECT_FALSE(c.rejected);
    BEZEL_EXPECT_FALSE(c.sanitizerFired);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, expected);
}

}  // namespace

// ===========================================================================
// THE PIN — #7246: a `Zip` whose combiner is a METHOD REFERENCE held in a
// local that holds another local. Before the fix: compiled clean, then an
// ASan SEGV in the generated program (the truncated element pointer). After
// the fix: it runs and returns the combined length.
// ===========================================================================
// The issue's own repro shape: `var f := CatMint; var g := f;` then
// `xs.Zip(ys, g)`.
BEZEL_TEST(lazy_local_of_local_method_reference_result_type_27_6_7246,
           zip_local_of_local_method_reference_combiner) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method CatMint(a: string, b: string): string => a + b; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    using var ys: Array<string> := [\"c\", \"dd\"]; "
        "    var f := CatMint; "
        "    var g := f; "
        "    var total: int := 0; "
        "    for var w in xs.Zip(ys, g) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectRuns(src, 6);  // "ac".Length + "bbdd".Length == 2 + 4
}

// The same shape through `Select` — the §5 projection, whose element slot is
// sized from the projection's result type the same way.
BEZEL_TEST(lazy_local_of_local_method_reference_result_type_27_6_7246,
           select_local_of_local_method_reference_projection) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method MakeMint(x: string): string => x + \"!\"; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    var f := MakeMint; "
        "    var g := f; "
        "    var total: int := 0; "
        "    for var w in xs.Select(g) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectRuns(src, 5);  // "a!".Length + "bb!".Length == 2 + 3
}

// A THREE-level chain — the walk is not bounded to two hops.
BEZEL_TEST(lazy_local_of_local_method_reference_result_type_27_6_7246,
           zip_three_level_alias_chain) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method CatMint(a: string, b: string): string => a + b; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    using var ys: Array<string> := [\"c\", \"dd\"]; "
        "    var f := CatMint; "
        "    var g := f; "
        "    var h := g; "
        "    var total: int := 0; "
        "    for var w in xs.Zip(ys, h) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectRuns(src, 6);
}

// The truncation is not specific to a MINTING combiner: a BORROWING one
// (the callback returns its argument) truncates the same way. The elements
// here are built at RUN time (`Join`), so they live on the heap — a
// static-literal element would sit below 4 GiB and survive the truncation
// by luck, which is why the row builds its elements at run time.
BEZEL_TEST(lazy_local_of_local_method_reference_result_type_27_6_7246,
           zip_local_of_local_borrowing_combiner_heap_elements) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method Join(a: string, b: string): string => a + b; "
        "  private static method Take(x: string): string => x; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [Join(\"a\", \"b\")]; "
        "    using var ys: Array<string> := [Join(\"c\", \"d\")]; "
        "    var f := Take; "
        "    var g := f; "
        "    var total: int := 0; "
        "    for var w in xs.Zip(ys, g) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectRuns(src, 2);  // "ab".Length == 2
}

// ===========================================================================
// CONTROLS — the neighbouring spellings must keep behaving EXACTLY as they
// did before the fix. A change that moved one of these is a behaviour flip.
// ===========================================================================
// The ANNOTATED local — `var f: (string, string) -> string := Cat;` —
// carries the Function TypeRef on the local itself, so it was lowered
// correctly before the fix and must still be. The alias of it (`var g := f;`)
// inherits that Function TypeRef, so it was lowered correctly too.
BEZEL_TEST(lazy_local_of_local_method_reference_result_type_27_6_7246,
           zip_annotated_local_alias_chain_unchanged) {
    std::string src =
        "module M; import System.Collections.Generic; "
        "import System.Collections.Linq; "
        "class C "
        "  private static method Cat(a: string, b: string): string => a; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    using var ys: Array<string> := [\"c\", \"dd\"]; "
        "    var f: (string, string) -> string := Cat; "
        "    var g := f; "
        "    var total: int := 0; "
        "    for var w in xs.Zip(ys, g) do "
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
BEZEL_TEST(lazy_local_of_local_method_reference_result_type_27_6_7246,
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
        std::fprintf(stderr, "[7246] the program was REJECTED at compile time\n");
    }
    if (c.sanitizerFired) {
        std::fprintf(stderr, "[7246] sanitizer report:\n%s\n", c.report.c_str());
    }
    BEZEL_EXPECT_FALSE(c.rejected);
    BEZEL_EXPECT_FALSE(c.sanitizerFired);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 3);
}
