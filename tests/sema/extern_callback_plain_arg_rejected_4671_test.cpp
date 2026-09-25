// fix #4671 / spec §32.8 (v0.307, line 9846) — at a plain (un-`env`-paired)
// function-typed extern parameter the callback must be capture-free (a static
// method reference or a non-capturing lambda). A bound instance-method
// reference and a capturing lambda each carry an environment a bare C
// function pointer cannot represent; the spec names two remedies (env-pair
// for call-bounded invocations, the registry idiom for retained ones).
//
// ROOT CAUSE. The `ExternDeclSymbol` arm of `ExprTC::typeCall`
// (src/sema/ExprTypecheck_Calls.cpp) wired up the call's return type and
// called `checkCallSiteInlineArrayArg` (sibling helper for §32.5 line 9706)
// but never consulted §32.8's plain-callback admission rule. The two
// corpus reject rows `s32_8_0003` (bound instance method) and `s32_8_0004`
// (capturing lambda) thus passed sema and reached codegen — the program
// compiled to a valid `bezelc` object file (exit 0) and would have emitted
// a C function-pointer address whose C-visible target — a callable
// constructed from `this.CompareInts` / a closure over `bias` — has no C
// representation at runtime.
//
// THE FIX adds one inline section in the `ExternDeclSymbol` arm. After
// visiting the arguments, it walks each non-env-marker function-typed
// extern parameter, skips env-paired slots (whose function type contains
// exactly one `env` position; §32.8 well-formedness rule 1), and reports
// the §32.8 reject when `isCaptureFreeEntry` returns false on the
// argument. The diagnostic names the spec's two remedies verbatim.
//
// Each test below compiles and links ONLY the call shape it owns; the
// `extern "libc"` declares `qsort` with a function-typed callback parameter,
// and the test's own `MainC` either names a bound instance-method, a
// static method ref, a capturing lambda, or a non-capturing lambda —
// exactly one admission shape per file. Putting all four shapes into
// one source would mean the reject rows always error the program at
// sema time even on the accept-row's static-ref call site — sema
// processes every parse-time call site, not just the runtime-reached
// ones, so the reject's diagnostic would short-circuit the accept's
// compile and r.ran would always read false.

#include <iostream>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Pinned on the spec's "capture-free" term (the wording both reject sites
// share with `isCaptureFreeEntry`) plus the section citation that
// authorises the rejection. The §32.8 plain-callback rule's distinct
// diagnostic must not be confused with the §22.12 static-event or
// §30.14 Domain.Spawn capture-free admissions — those have their own
// scope and their own diagnostic wordings, none of which combine
// `plain`/`env`-paired`/`capture-free`/`32.8`.
bool hasPlainCallbackEnvRejection(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("plain") != std::string::npos &&
            d.message.find("env") != std::string::npos &&
            d.message.find("capture-free") != std::string::npos &&
            d.message.find("32.8") != std::string::npos)
            return true;
    return false;
}

// Shared `extern "libc"` declaration of `qsort` with a function-typed
// `compar` parameter — the §32.8 surface this fix narrows.
const char kExternQsort[] =
    "module Conf; "
    "extern \"libc\" "
    "  method qsort(basePtr: IntPtr, nmemb: UIntPtr, size: UIntPtr, "
    "      compar: (IntPtr, IntPtr) -> int): (); "
    "end; ";

// Wraps a method class body in `class MainC { ... }` whose `Main` calls
// the row's own call site. Each row passes its own class body so the
// reject / accept verdict lands cleanly on the call shape the row tests.
JitResult runWith(const std::string& classBody,
                 const std::string& mainBody) {
    return runAot(std::string(kExternQsort) + classBody +
                  "public static class MainC "
                  "  public static method Main(): int " +
                  mainBody + " end Main; "
                  "end MainC;");
}

}  // namespace

// ============================================================================
// REJECT ROWS — the spec-strict §32.8 violation. The two rows here are the
// issue's literal repros (s32_8_0003 and s32_8_0004); pre-fix they
// compiled clean (the fix changed §32.8 to a hard reject at the call
// site).
// ============================================================================

// Row 1 — bound instance-method reference at a plain extern callback
// parameter. `this.CompareInts` carries the receiver; a bare C function
// pointer has no slot for it. Mirrors the s32_8_0003 corpus row.
BEZEL_TEST(extern_callback_plain_arg_rejected_4671,
           bound_instance_method_at_plain_callback_rejected_spec_32_8) {
    auto r = runWith(
        "public class Sorter "
        "  public constructor() end constructor; "
        "  public method CompareInts(a: IntPtr, b: IntPtr): int "
        "    return 0; "
        "  end CompareInts; "
        "  public method SortInPlace(arrayPtr: IntPtr, count: UIntPtr, "
        "      sizeOfInt: UIntPtr) "
        "    qsort(arrayPtr, count, sizeOfInt, this.CompareInts); "
        "  end SortInPlace; "
        "end Sorter; ",
        "var s := new Sorter(); "
        "s.SortInPlace(IntPtr.Zero, UIntPtr.Zero, UIntPtr.Zero); "
        "return 0;");
    if (!r.diagnostics.hasErrors()) {
        BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    }
    BEZEL_EXPECT_TRUE(hasPlainCallbackEnvRejection(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Row 2 — capturing lambda at a plain extern callback parameter. The
// lambda's `bias` capture is a borrow of the callee frame; a bare C
// function pointer has no slot for it. Mirrors the s32_8_0004 corpus row.
BEZEL_TEST(extern_callback_plain_arg_rejected_4671,
           capturing_lambda_at_plain_callback_rejected_spec_32_8) {
    auto r = runWith(
        "public static class IntSorterBias "
        "  public static method SortWithBias(arrayPtr: IntPtr, count: UIntPtr, "
        "      sizeOfInt: UIntPtr, bias: int) "
        "    qsort(arrayPtr, count, sizeOfInt, "
        "        lambda(a: IntPtr, b: IntPtr): int => bias); "
        "  end SortWithBias; "
        "end IntSorterBias; ",
        "IntSorterBias.SortWithBias(IntPtr.Zero, UIntPtr.Zero, "
        "    UIntPtr.Zero, 0); return 0;");
    if (!r.diagnostics.hasErrors()) {
        BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    }
    BEZEL_EXPECT_TRUE(hasPlainCallbackEnvRejection(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================================
// CONTROL ROWS — what stays ACCEPTED. The check discriminates on the
// argument's "capture-freeness", so a static method reference (which
// `isCaptureFreeEntry` keys as a MemberAccessExpr whose target resolves
// to a TypeSymbol) and a non-capturing lambda (whose body references no
// enclosing local/parameter) are exactly the two §32.8 admits at the
// plain callback parameter.
// ============================================================================

// Row 3 — static method reference at the plain extern callback. The
// qualifier `IntSorter.CompareInts` resolves its target as a TypeSymbol,
// so `isCaptureFreeEntry` returns true and the call is admitted. This is
// the same shape as `s32_8_0001` in the corpus; pre-fix and post-fix it
// must stay green.
BEZEL_TEST(extern_callback_plain_arg_rejected_4671,
           static_method_ref_at_plain_callback_accepted_spec_32_8) {
    auto r = runWith(
        "public static class IntSorter "
        "  public static method CompareInts(a: IntPtr, b: IntPtr): int "
        "    return 0; "
        "  end CompareInts; "
        "  public static method SortInPlace(arrayPtr: IntPtr, count: UIntPtr, "
        "      sizeOfInt: UIntPtr) "
        "    qsort(arrayPtr, count, sizeOfInt, IntSorter.CompareInts); "
        "  end SortInPlace; "
        "end IntSorter; ",
        "IntSorter.SortInPlace(IntPtr.Zero, UIntPtr.Zero, UIntPtr.Zero); "
        "return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 4 — non-capturing lambda at the plain extern callback. The lambda
// body `int => 0` references no enclosing local/parameter, so
// `lambdaBodyCaptures` returns false and `isCaptureFreeEntry` returns
// true. Same shape as `s32_8_0002`; pre-fix and post-fix must stay
// green.
BEZEL_TEST(extern_callback_plain_arg_rejected_4671,
           non_capturing_lambda_at_plain_callback_accepted_spec_32_8) {
    auto r = runWith(
        "public static class IntSorterPlain "
        "  public static method SortPlain(arrayPtr: IntPtr, count: UIntPtr, "
        "      sizeOfInt: UIntPtr) "
        "    qsort(arrayPtr, count, sizeOfInt, "
        "        lambda(a: IntPtr, b: IntPtr): int => 0); "
        "  end SortPlain; "
        "end IntSorterPlain; ",
        "IntSorterPlain.SortPlain(IntPtr.Zero, UIntPtr.Zero, UIntPtr.Zero); "
        "return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
