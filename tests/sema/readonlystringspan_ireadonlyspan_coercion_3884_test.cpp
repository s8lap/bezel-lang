// Sema test — issue #3884: a `ReadOnlyStringSpan` value silently coerced to
// the `IReadOnlySpan<char>` interface reaches codegen with no boxing at all,
// producing either a wrong runtime answer (no diagnostic) or a codegen crash,
// depending on how the interface-typed slot is used.
//
// THE GOVERNING AUTHORITY.
//
// stdlib §3.3 declares `ReadOnlyStringSpan` implements `IReadOnlySpan<char>`:
//   `public record ReadOnlyStringSpan(Source: string, Start: int,
//    Length: int): IReadOnlySpan<char>`
// — so the language DOES intend this conformance to eventually work (issues
// 2313 / 2317 track making it actually function). But unlike the four
// buffer/array-backed spans (`ArraySpan<T>`/`ReadOnlyArraySpan<T>`/`Span<T>`/
// `ReadOnlySpan<T>`), `ReadOnlyStringSpan` does NOT share their
// `bz.arrayspan` { ptr, i32, i32 } layout — its backing `string` uses the
// §4.6 adaptive 1/2/4-byte code-point encoding — so `isSpanLikeTypeName`
// (src/codegen/LlvmBackend_impl.hpp) deliberately excludes it and no boxed
// interface-span representation exists for it in codegen today. Given that,
// the compiler owes a REJECTION of the coercion (CLAUDE.md's "decline with
// reasons"), not a silent accept that reaches an unimplemented codegen path
// two different ways:
//
//   * `var iface: IReadOnlySpan<char> := aReadOnlyStringSpan;` compiled and
//     ran with NO diagnostic; `iface.SequenceEqual(other)` then silently
//     returned the WRONG boolean (`false` for two spans over equal strings)
//     — issue #3884, the defect this test pins.
//   * `Len(sp: IReadOnlySpan<char>)` called as `Len(s.AsSpan())` reached
//     codegen's real call-site boxing path, which assumes the
//     `bz.arrayspan` layout, and crashed: "Call parameter type does not
//     match function signature!" / IR verification failure — issue #2313,
//     the same root defect through the parameter-passing door.
//
// ROOT CAUSE. `ExprTC::checkAssignable` (src/sema/ExprTypecheck.cpp) has an
// explicit §4.16/§8.6 boxing-coercion arm for the four buffer/array spans
// coercing to `IReadOnlySpan<T>`, but `ReadOnlyStringSpan` matched none of
// its `fromN` arms and fell through every remaining branch in the function
// to its final catch-all ("Mixed / generic-parameter / null-literal cases:
// skip") — i.e. silently ACCEPTED with no boxing wired up at all.
//
// THE FIX. An explicit `fromN == "ReadOnlyStringSpan"` arm in that same
// boxing-coercion block, rejecting the coercion with a diagnostic that names
// the missing boxed representation and points at the working alternatives —
// mirroring the "not yet supported" style ExprTypecheck_Calls.cpp already
// uses for the direct-receiver argument form of this exact gap
// (`SequenceEqual(other: IReadOnlySpan<char>)` on a `ReadOnlyStringSpan`
// receiver, and `CopyTo` beside it).
//
// SCOPE. This does NOT implement the feature: issue 2317 remains open to
// make the interface form of `SequenceEqual` (and general IReadOnlySpan<char>
// parameter passing, issue 2313) actually work once a boxed representation
// for `ReadOnlyStringSpan` exists. This closes only the silent-wrong-answer /
// malformed-IR gap by refusing the coercion at compile time instead.
//
// FAIL-WITHOUT / PASS-WITH (measured against this lane's base commit
// f19de07, PARENT bezelc built from this same worktree with the
// checkAssignable change reverted, test binary relinked before each probe):
//   * `readonly_stringspan_local_coercion_rejected_3884`: PARENT compiles the
//     program with ZERO diagnostics and RUNS it to exit code 0 (the wrong
//     answer — equal strings' SequenceEqual reads false) — this test's
//     `hasReadOnlyStringSpanCoercionError` predicate is FALSE (r.ran==true,
//     no matching error). FIXED compiler rejects at compile time with the
//     diagnostic asserted below — predicate TRUE.
//   * `readonly_stringspan_parameter_coercion_rejected_2313`: PARENT compiles
//     to codegen and dies with "IR verification failed" (issue #2313's own
//     repro) — `r.ran` is false, but the message does NOT contain this
//     fix's diagnostic text, so the predicate is FALSE (proving the row does
//     not vacuously pass on a differently-worded reject). FIXED compiler
//     rejects at compile time with the same diagnostic — predicate TRUE.

#include <cctype>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace bezel::sema {
namespace {

// True iff an ERROR diagnostic is THIS fix's ReadOnlyStringSpan ->
// IReadOnlySpan<char> coercion rejection. Checked by distinctive substrings
// rather than the whole message so Pass-1/other-callsite wording variance
// (the `what` fragment differs between "the initializer for `iface`" and
// "the argument `sp` to call to `Len`") does not make the predicate brittle.
// Deliberately NOT a bare `!r.ran` (LANE_RULES §3): a runtime crash also
// has `r.ran == false`, and issue #2313's own pre-fix repro dies with an
// UNRELATED "IR verification failed" message this predicate must reject.
bool hasReadOnlyStringSpanCoercionError(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const std::string& m = d.message;
        if (m.find("ReadOnlyStringSpan") == std::string::npos) continue;
        if (m.find("IReadOnlySpan<char>") == std::string::npos) continue;
        if (m.find("\xC2\xA7""4.16") == std::string::npos) continue;
        if (m.find("boxed interface-span representation") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

// The rejection's diagnostic must carry a REAL source location, not the
// synthetic {0,0} `runAot` packs a runtime-crash report into (LANE_RULES
// §3): parse the `<path>:<line>:<col>:` prefix out of the message text
// (mirrors tests/codegen/encoding_test.cpp's `hasErrorLocatedAt`).
bool hasRealLocationPrefix(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        std::size_t colon1 = m.find(".bzl:");
        if (colon1 == std::string::npos) continue;
        std::size_t p = colon1 + 5;  // just past ".bzl:"
        if (p >= m.size() || !std::isdigit(static_cast<unsigned char>(m[p])))
            continue;
        return true;  // a digit right after ".bzl:" is a real line number
    }
    return false;
}

// Compiles, links, runs, and returns the exit code (-999 on any failure).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =============================================================================
// REJECT — the #3884 defect: a local variable's declared-type coercion.
// =============================================================================

// §3.3 / §4.16, issue #3884: `var iface: IReadOnlySpan<char> := <a
// ReadOnlyStringSpan>;` must be a compile-time rejection, not a silent
// accept that later dispatches `.SequenceEqual` through an unboxed struct.
BEZEL_TEST(sema_readonlystringspan_ireadonlyspan_coercion_3884,
           readonly_stringspan_local_coercion_rejected_3884) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    var other := \"hello\"; "
        "    var iface: IReadOnlySpan<char> := s.AsSpan(); "
        "    if iface.SequenceEqual(other) then "
        "      return 1; "
        "    else "
        "      return 0; "
        "    end if; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReadOnlyStringSpanCoercionError(r));
    BEZEL_EXPECT_TRUE(hasRealLocationPrefix(r));
}

// =============================================================================
// REJECT — the adjacent #2313 defect (parameter passing): SAME root cause,
// so the SAME fix converts its "malformed IR" codegen crash into a clean
// compile-time reject with this test's diagnostic — never a differently-
// worded reject and never a runtime crash.
// =============================================================================

// Issue #2313's own repro shape: an `IReadOnlySpan<char>` parameter fed a
// `ReadOnlyStringSpan` argument (`s.AsSpan()`). Pre-fix this reached codegen
// and died with "Call parameter type does not match function signature!" /
// IR verification failure; the fix rejects it at compile time instead, with
// the SAME diagnostic as the local-coercion case above (both are the same
// `checkAssignable` arm — a call argument is checked the same way as an
// initializer).
BEZEL_TEST(sema_readonlystringspan_ireadonlyspan_coercion_3884,
           readonly_stringspan_parameter_coercion_rejected_2313) {
    auto r = runAot(
        "module M; class C "
        "  private static method Len(sp: IReadOnlySpan<char>): int "
        "    return sp.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    return Len(s.AsSpan()); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReadOnlyStringSpanCoercionError(r));
}

// =============================================================================
// REGRESSION — the ACCEPTED span types (§4.16/#970) must keep working, with
// correct VALUES, not just "compiles". Only ReadOnlyStringSpan gains a new
// rejection; ArraySpan/ReadOnlyArraySpan/Span/ReadOnlySpan are untouched.
// =============================================================================

// §4.16 (#970): a `ReadOnlyArraySpan<int>` still boxes into
// `IReadOnlySpan<int>` at a call site and dispatches Length/indexer
// correctly — this fix's new `ReadOnlyStringSpan` arm sits in the same
// `if` block but is gated on a DIFFERENT `fromN`, so this sibling case must
// be unaffected.
BEZEL_TEST(sema_readonlystringspan_ireadonlyspan_coercion_3884,
           array_span_coercion_still_accepted_regression_970) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Sum(span: IReadOnlySpan<int>): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < span.Length do "
        "      total := total + span[i]; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(3); "
        "    arr.Add(1); arr.Add(2); arr.Add(4); "
        "    return C.Sum(arr.AsReadOnlySpan()); "
        "  end Main; "
        "end C;"),
        7);
}

// Issue #2317's PRE-EXISTING direct-receiver-argument diagnostic (a
// `ReadOnlyStringSpan`-typed RECEIVER calling `.SequenceEqual` with an
// `IReadOnlySpan<char>` ARGUMENT) is a completely different code path
// (ExprTypecheck_Calls.cpp's `rcn == "ReadOnlyStringSpan"` member-call
// block, not `checkAssignable`) and must keep emitting ITS OWN wording,
// unreplaced by this fix's coercion diagnostic.
BEZEL_TEST(sema_readonlystringspan_ireadonlyspan_coercion_3884,
           direct_receiver_argument_diagnostic_unaffected_regression_2317) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        // §4.16/#970's ACCEPTED coercion (ReadOnlyArraySpan<char>, not
        // ReadOnlyStringSpan) — deliberately avoids also tripping this fix's
        // own new rejection, so this row isolates #2317's diagnostic alone.
        "    var chars := new Array<char>(1); chars.Add('x'); "
        "    var iface: IReadOnlySpan<char> := chars.AsReadOnlySpan(); "
        "    var direct := s.AsSpan(); "
        "    if direct.SequenceEqual(iface) then return 1; else return 0; end if; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool hasIssue2317Wording = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not yet supported") != std::string::npos &&
            d.message.find("boxed interface-span operand") !=
                std::string::npos) {
            hasIssue2317Wording = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(hasIssue2317Wording);
}

}  // namespace bezel::sema
