// Codegen regression test for issue #8959 — §21.9 *The statement form of
// expressions* / §28.3.11 *Unbound owned temporaries* / §17.13 *The discard
// `_`* / §35.4 *The classification* (spec v0.359).
//
// #8959 — `[codegen] an expression statement whose call returns an owning
// choice leaks the result: `h.Make();`, `h.Pass(move s);``.
//
// §28.3.11 (v0.359): "An owned value may also be produced as an **unbound
// temporary**: the result of an `owned`-returning call (or other owned
// producer) that is not bound to any name, not `move`d, and not returned …
// Such a temporary is disposed at the **end of the full expression** in which
// it is produced, after every use of it within that expression has
// completed" — and §21.9 fixes the boundary for an expression statement
// ("in an expression statement, 'end of the full expression' and 'end of the
// statement' coincide"). The bare expression statement is ADMITTED (with
// §21.9's unused-result warning), so its temporary must still be disposed.
//
// §35.3 says a method returning an owning choice writes the PLAIN type — "`owned`
// on the return is admitted-but-redundant per §28.1's move-only clause" — so
// the §21.9 `ownedProduced` marker never fires for the shape, and the
// class-only `ExpressionStmt::disposeOwnedValue` arm could not see it. The
// result was produced and dropped on the floor: every iteration leaked the
// choice's active case's `owned` payload.
//
// THE ORACLE. A COUNTING destructor gives one increment per disposal, so a
// suppressed disposal (a leak) is distinguishable from a doubled one (a double
// free) and from the spec's answer — the same oracle the neighbouring
// §17.13 discard rows use (`owning_choice_discard_and_bang_field_store_7861_7858`).
// The rows run on `runAotChecked`, because a leak row must be on the
// ASan/LSan-instrumented oracle (tests/README.md, *Which helper?*): `runAot`
// does not leak-check at all, so a loop-amplified leak on it would pass on
// BOTH sides of the fix. `assertNoLeak` adds the byte/allocation count, and
// `checkedToolchainSkipped` makes a missing sanitizer toolchain LOUD instead
// of a silent pass.
//
// MEASURED on the base (r43-f45-toolchain @ 0efd9b0ca), 200 iterations:
//   `h.Make();`                → LeakSanitizer "3200 byte(s) leaked in 200
//                                allocation(s)"; the count printed `0`;
//   `h.Pass(move s);`          → the same;
//   `h?.Make();`               → "2128 byte(s) leaked in 133 allocation(s)";
//   `OChoice.Text(new Res());` → "3200 byte(s) leaked in 200 allocation(s)".
// The `_ := h.Make();` discard spelling was ALREADY clean, which is the
// asymmetry that identifies the defect: the reclaim fired when the temporary
// was CONSUMED and not when it was discarded entirely.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output, and
// neither AddressSanitizer nor the runtime's §28.9 over-release check fired.
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

// The issue's own repro, VERBATIM: the counting `Resource` of the neighbouring
// §17.13 discard rows, the owning choice `Bundle`, and the two spellings the
// issue names.
const char* kDecls =
    "class Resource "
    "  public static Count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Resource.Count := Resource.Count + 1; end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; "
    "class Holder "
    "  public constructor() end constructor; "
    "  public method Make(): Bundle return Bundle.Ready(new Resource()); end Make; "
    "  public method Pass(owned o: Bundle): Bundle return o; end Pass; "
    "end Holder; ";

JitResult run(const std::string& body) {
    return runAotChecked(std::string("module M; ") + kDecls +
                         " class Program public static method Main() " + body +
                         " end Main; end Program;");
}

}  // namespace

// --- #8959: the bare expression statement of an owning-choice production -----

BEZEL_TEST(codegen_owning_choice_expression_statement_disposal_8959,
           spec_28_3_11_a_bare_expression_statement_of_an_owning_choice_call_disposes_its_payload) {
    // The issue's literal repro: a 200-iteration loop with BOTH spellings the
    // issue names — `h.Make();` and `h.Pass(move s);`. Each iteration
    // constructs TWO `Resource`s: the one `Make` mints and the one `s` holds
    // (which `Pass` takes and hands back). BOTH must be disposed exactly once,
    // so the count is `2` per iteration.
    //
    // Pre-fix: `0` — the `Make` payload leaked and `s`'s payload was moved
    // into the discarded result, so it leaked too; LeakSanitizer reported
    // "6400 byte(s) leaked in 400 allocation(s)".
    expectClean(
        run("var i := 0; "
            "while i < 200 do "
            "  var h := new Holder(); "
            "  h.Make(); "
            "  var s := Bundle.Ready(new Resource()); "
            "  h.Pass(move s); "
            "  i := i + 1; "
            "end while; "
            "Console.WriteLine($\"{Resource.Count}\");"),
        0, std::string("400\n"));
}

BEZEL_TEST(codegen_owning_choice_expression_statement_disposal_8959,
           spec_28_3_11_the_null_conditional_spelling_of_the_same_statement) {
    // The issue's second spelling family: `h?.Make();`. §7.5 makes the `?.`
    // access ONE access — "it performs the access" — so the owning choice it
    // produces is the same unbound owned temporary the `.` spelling produces,
    // and the same §28.3.11 boundary applies.
    //
    // Pre-fix: `0` — LeakSanitizer reported "2128 byte(s) leaked in 133
    // allocation(s)" over the issue's 200-iteration loop (the 133 non-null
    // iterations of a loop that nulls the receiver every third time).
    expectClean(
        run("var i := 0; "
            "while i < 200 do "
            "  var h: Holder? := new Holder(); "
            "  h?.Make(); "
            "  i := i + 1; "
            "end while; "
            "Console.WriteLine($\"{Resource.Count}\");"),
        0, std::string("200\n"));
}

BEZEL_TEST(codegen_owning_choice_expression_statement_disposal_8959,
           spec_35_3_a_bare_case_construction_statement_disposes_its_payload) {
    // §35.3: "a construction not bound to any name is an unbound owned
    // temporary under K8's full-expression machinery". A case construction is
    // a CALL (the callee resolves to the choice's case symbol), so the same
    // §28.3.11 boundary applies to it.
    //
    // Pre-fix: `0` — LeakSanitizer reported "3200 byte(s) leaked in 200
    // allocation(s)".
    expectClean(
        run("var i := 0; "
            "while i < 200 do "
            "  Bundle.Ready(new Resource()); "
            "  i := i + 1; "
            "end while; "
            "Console.WriteLine($\"{Resource.Count}\");"),
        0, std::string("200\n"));
}
