// issue #7977: "a lambda that `move`s a captured variable and is called twice
// is a use-after-free". MEASURED by the R37 review r37-rev-l5f; its probes are
// /var/tmp/bezel-scratch/r37-rev-l5f/probes/r25*.bzl, reproduced verbatim (as
// C++ literals) below.
//
// THE TWO SHAPES THE ISSUE'S REPROS CARRY, AND WHAT THE SPEC SAYS OF EACH.
//
// (1) `var x := G.It(); const mk := lambda(): IEnumerator<int> => move x;`
//     — a marked transfer from a CAPTURED OWNED LOCAL. §27.5 (v0.357): "For a
//     captured owned binding the sharing is read-only in the ownership sense:
//     the lambda may not assign to it, transfer from it, or dispose it";
//     §28.3 *Captured owned bindings* (v0.357): "a marked transfer from it ...
//     [is a] compile error". So the second call never happens: the program is
//     REJECTED. The reviewer's base (26216e9f) accepted it; the current tree
//     rejects it, and the `rejects_*` row below pins that verdict.
//
// (2) `const mk := lambda(): IEnumerator<int>? => move h.En;` over a captured
//     `using var h` — a take of a FIELD through a captured holder. That is not
//     a transfer from the binding: §28.8.1 (v0.357) admits "any object
//     expression" as the receiver of `move x.f`, "a borrowed parameter
//     included", and a bare take at a nullable field "stores `null`" — "a
//     later read reads `null`". The slot is `IEnumerator<int>?`, a
//     mint-announcing return (§28.3 *Callee-side admission*: an owned value
//     is admitted), so the program is LEGAL and must be SAFE: the first call
//     yields the frame and leaves `null`, the second yields `null`.
//
// THE DEFECT (shape 2) IS NOT LAMBDA-SPECIFIC. The bare take over an `owned`
// field typed `IEnumerator<T>?` never stored `null`, so ANY second take —
// `var p := move h.En; var q := move h.En;`, with no lambda at all — handed
// the same iterator frame to two owners, and the second Dispose read freed
// memory (ASan: heap-use-after-free, freed in `bz$iter$...$CloneDispose`).
// lowerUnary's field-slot nulling gated on isOwnedIteratorInterfaceField, the
// owned-field DISPOSAL predicate, which admits only the sync enumerable and
// the async enumerator heads; `IEnumerator<T>`, `IOwningEnumerator<T>`,
// `IAsyncEnumerable<T>` and `IOwningAsyncEnumerable<T>` were skipped.
//
// FAIL-WITHOUT: with src/codegen/LlvmBackend_Exprs2.cpp reverted, every row
// here except `rejects_*` and the `IEnumerable` control fails (MEASURED; see
// the commit body).
//
// per §28.8.1, §28.3, §27.5, v0.357

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasSanitizerHeadline(const std::string& text) {
    static const char* markers[] = {
        "ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
        "SUMMARY: AddressSanitizer", "detected memory leaks",
    };
    for (const char* m : markers)
        if (text.find(m) != std::string::npos) return true;
    return false;
}

std::size_t countLines(const std::string& text, const std::string& line) {
    std::size_t n = 0, pos = 0;
    const std::string needle = line + "\n";
    if (text.compare(0, needle.size(), needle) == 0) ++n;
    const std::string inner = "\n" + needle;
    while ((pos = text.find(inner, pos)) != std::string::npos) {
        ++n;
        pos += 1;
    }
    return n;
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The reviewer's r25* scaffold, verbatim: `G.It()` is a suspended-capable
// enumerator whose `finally` prints `fin`; `H.En` is the owned
// `IEnumerator<int>?` field, filled from `G.Gen().GetEnumerator()` and
// advanced once, so disposing it runs the `finally` (§29.7).
const char* kScaffold =
    "module M; "
    "class G "
    "  public static method Gen(): IEnumerable<int> "
    "    try "
    "      yield return 1; "
    "      yield return 2; "
    "    finally "
    "      Console.WriteLine(\"fin\"); "
    "    end; "
    "  end Gen; "
    "  public static method It(): IEnumerator<int> "
    "    try "
    "      yield return 5; "
    "      yield return 6; "
    "    finally "
    "      Console.WriteLine(\"fin\"); "
    "    end; "
    "  end It; "
    "end G; "
    "class H "
    "  public owned En: IEnumerator<int>? := null; "
    "  public method Fill() "
    "    En := G.Gen().GetEnumerator(); "
    "  end Fill; "
    "  public method TakeBareTwice() "
    "    var p := move En; var q := move En; p?.Dispose(); q?.Dispose(); "
    "  end TakeBareTwice; "
    "  public method TakeThisTwice() "
    "    var p := move this.En; var q := move this.En; "
    "    p?.Dispose(); q?.Dispose(); "
    "  end TakeThisTwice; "
    "  public static method TakeParamTwice(h: H) "
    "    var p := move h.En; var q := move h.En; p?.Dispose(); q?.Dispose(); "
    "  end TakeParamTwice; "
    "end H; ";

// 100 iterations, as the reviewer's probe: each one fills `h.En`, advances it
// once, runs `body`, and lets `using var h` dispose the holder.
std::string loopProgram(const std::string& body) {
    return std::string(kScaffold) +
           "class C "
           "  public static method Main(): int "
           "    var i := 0; "
           "    while i < 100 do "
           "      using var h := new H(); h.Fill(); "
           "      var ok := h.En!.MoveNext(); " +
           body +
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(\"end\"); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The frame each iteration takes is disposed exactly once: its `finally`
// prints `fin` once per iteration, the run completes, and the sanitizer is
// silent. A frame handed to two owners prints `fin` once and then faults.
void expectEachFrameDisposedOnce(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerHeadline(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countLines(r.stdoutText, "fin"), static_cast<std::size_t>(100));
    BEZEL_EXPECT_EQ(countLines(r.stdoutText, "end"), static_cast<std::size_t>(1));
}

}  // namespace

// The issue's probe r25c, verbatim: the lambda takes a field of the captured
// `using var h` and is invoked twice. Legal (§28.8.1); the second invocation
// must yield `null`.
BEZEL_TEST(move_iter_format_field_bare_take_7977,
           issue_repro_lambda_takes_captured_holders_field_twice_28_8_1) {
    JitResult r = runAotChecked(loopProgram(
        "      const mk := lambda(): IEnumerator<int>? => move h.En; "
        "      var p := mk(); var q := mk(); p?.Dispose(); q?.Dispose(); "));
    expectEachFrameDisposedOnce(r);
}

// The same take, block-bodied: the lambda form is not what decides it.
BEZEL_TEST(move_iter_format_field_bare_take_7977,
           block_bodied_lambda_takes_field_twice_28_8_1) {
    JitResult r = runAotChecked(loopProgram(
        "      const mk := lambda(): IEnumerator<int>? => do "
        "        return move h.En; "
        "      end; "
        "      var p := mk(); var q := mk(); p?.Dispose(); q?.Dispose(); "));
    expectEachFrameDisposedOnce(r);
}

// No lambda at all: two bare takes through a local holder.
BEZEL_TEST(move_iter_format_field_bare_take_7977,
           direct_double_take_through_local_holder_28_8_1) {
    JitResult r = runAotChecked(loopProgram(
        "      var p := move h.En; var q := move h.En; "
        "      p?.Dispose(); q?.Dispose(); "));
    expectEachFrameDisposedOnce(r);
}

// The three other receiver spellings lowerUnary distinguishes: the implicit
// field (`move En`), the `this`-qualified field, and a borrowed parameter's
// field (§28.8.1: "a borrowed parameter included").
BEZEL_TEST(move_iter_format_field_bare_take_7977,
           double_take_implicit_field_28_8_1) {
    JitResult r = runAotChecked(loopProgram("      h.TakeBareTwice(); "));
    expectEachFrameDisposedOnce(r);
}

BEZEL_TEST(move_iter_format_field_bare_take_7977,
           double_take_this_qualified_field_28_8_1) {
    JitResult r = runAotChecked(loopProgram("      h.TakeThisTwice(); "));
    expectEachFrameDisposedOnce(r);
}

BEZEL_TEST(move_iter_format_field_bare_take_7977,
           double_take_borrowed_parameter_field_28_8_1) {
    JitResult r = runAotChecked(loopProgram("      H.TakeParamTwice(h); "));
    expectEachFrameDisposedOnce(r);
}

// The value the spec states directly: "a later read reads `null`". Prints
// `True` after the take; the defect printed `False` (the field kept the
// frame). Independent of any sanitizer.
BEZEL_TEST(move_iter_format_field_bare_take_7977,
           field_reads_null_after_bare_take_28_8_1) {
    JitResult r = runAotChecked(loopProgram(
        "      var p := move h.En; Console.WriteLine(h.En is null); "
        "      p?.Dispose(); "));
    expectEachFrameDisposedOnce(r);
    BEZEL_EXPECT_EQ(countLines(r.stdoutText, "True"), static_cast<std::size_t>(100));
    BEZEL_EXPECT_EQ(countLines(r.stdoutText, "False"), static_cast<std::size_t>(0));
}

// Positive control: an `owned IEnumerable<int>?` field was already vacated by
// a take (the head the disposal predicate admits). Green before and after.
BEZEL_TEST(move_iter_format_field_bare_take_7977,
           control_enumerable_field_double_take_28_8_1) {
    const std::string src =
        "module M; "
        "class G "
        "  public static method Gen(): IEnumerable<int> "
        "    try "
        "      yield return 1; "
        "    finally "
        "      Console.WriteLine(\"fin\"); "
        "    end; "
        "  end Gen; "
        "end G; "
        "class H "
        "  public owned Eb: IEnumerable<int>? := null; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 100 do "
        "      using var h := new H(); h.Eb := G.Gen(); "
        "      var a := move h.Eb; Console.WriteLine(h.Eb is null); "
        "      var b := move h.Eb; a?.Dispose(); b?.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(\"end\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerHeadline(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countLines(r.stdoutText, "True"), static_cast<std::size_t>(100));
    BEZEL_EXPECT_EQ(countLines(r.stdoutText, "end"), static_cast<std::size_t>(1));
}

// The issue's probe r25, verbatim: a marked transfer from a captured owned
// LOCAL. §27.5 / §28.3 *Captured owned bindings* (v0.357) make it a compile
// error at the `move x`, so the second call never runs.
BEZEL_TEST(move_iter_format_field_bare_take_7977,
           rejects_issue_repro_lambda_moves_captured_owned_local_28_3) {
    JitResult r = runAot(loopProgram(
        "      var x := G.It(); "
        "      const mk := lambda(): IEnumerator<int> => move x; "
        "      var p := mk(); var q := mk(); p.Dispose(); q.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "cannot transfer captured owned binding `x` inside a closure"));
}
