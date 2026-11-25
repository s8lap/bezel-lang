// Codegen — issue #9320: a GENERIC `Array<T>.Add(x)` at T := a function type
// leaked one closure per call (64 B x 200). The NON-generic spelling of the
// same program is a §28.14 compile error, so the generic helper was the only
// way to write it — and it leaked.
//
// THE RULE. §28.11 (v0.359) "Collection element ownership" *The element slot
// policy* puts a FUNCTION type in the *runtime-managed reference* row — with
// `string` and `Type` — whose borrowing-family column reads "shared
// references": the store "stores the reference" — the collection owns a
// reference of ITS OWN on the element it stores — and the operations table
// gives that reference back when the slot goes away ("Remove / RemoveAt:
// removes the reference", "Clear(): removes all", the collection's
// `Dispose()`: "no user-visible work", the references it holds go back
// invisibly). §28.9 (v0.359) "Runtime-managed types" puts a function-typed
// value in the runtime-managed tier, whose "heap-allocated closure object"
// is reference-counted, and §27.5 (v0.359) keeps a closure's captures alive
// as long as the closure. §14.1 (v0.359) fixes the element category at the
// INSTANTIATION (§3.6 rule 2), so the slot's take and its give-back must read
// the SUBSTITUTED element type.
//
// ROOT CAUSE (measured, not assumed — the issue's own analysis is wrong on
// both halves). The collection's slot DOES take its own reference (the #9104
// take, resolved at the monomorphization), and the collection's free DOES
// give it back; what was missing is the CALLER's own reference. The
// `objdump` of the two ASan binaries: the generic program's `main` carried
// ZERO `bz_closure_release` call sites, while the concrete twin's `main`
// carried one — the caller's scope-end release was never armed.
//
// The escape walker (`ClosureEscapeWalker`,
// src/codegen/LlvmBackend_Setup.cpp) classifies a closure local passed to a
// call as an ESCAPE unless the callee provably keeps nothing of it. For a
// bare type-parameter formal (`closureArgBindsMethodOwnTypeParamBorrow`) the
// proof is the fail-closed callee-body walk (`FormalInvocationOnlyWalk`),
// which admits the formal's use as an invocation, an inspection, a store into
// a TAKING FIELD (the #9112 fix-forward) or a forward — but NOT a store into
// a collection. So `xs.Add(x)` into an `Array<T>` PARAMETER left the argument
// an ESCAPE, the local's scope-end release was never armed, and the caller's
// reference was stranded: the closure's count ended at 1 and its cell leaked.
//
// The DIRECT spelling (`arr.Add(f)`) was admitted by
// `isBuiltinArrayFunctionElementStore` (the #9104/#9112 relief) — the
// GENERIC twin was missing.
//
// MEASURED on the base (r30-c1 @ d66898662, `bezelc --sanitize=address`,
// LSan on, 200 iterations):
//   G.AddTo<T>(xs: Array<T>, x: T) at T := () -> int
//     -> "SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s)."
//   the concrete `arr.Add(f)` twin -> clean.
//
// Every program runs its body 200 times under runAotChecked (generated-code
// AddressSanitizer + LeakSanitizer), so a leak (the bug), a double-free and a
// use-after-free all flip `r.ran`; stdout pins the values.

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
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
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

// THE ISSUE'S REPRO, VERBATIM: a generic helper that stores its `T` argument
// into an `Array<T>`, called with T := a function type. The helper's body is
// checked against an unconstrained `T`, so §28.14's call-bounded rule is not
// applied to it — the program is accepted, and an accepted program must not
// leak.
//
// RED on the base: 64 B leaked per iteration (200 iterations ⇒ 12800 B in
// 200 allocations). GREEN with the fix: prints 400, exits 0, leaks nothing.
const char* kGenericAddToFunctionElement =
    "module App;\n"
    "import System.Collections;\n"
    "public class G\n"
    "  public static method AddTo<T>(xs: Array<T>, x: T)\n"
    "    xs.Add(x);\n"
    "  end AddTo;\n"
    "end G;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var acc := 0;\n"
    "    for var i in 0 ..< 200 do\n"
    "      using var arr: Array<() -> int> := new Array<() -> int>();\n"
    "      do\n"
    "        var f: () -> int := lambda() => 2;\n"
    "        G.AddTo(arr, f);\n"
    "      end;\n"
    "      acc := acc + arr[0]();\n"
    "    end for;\n"
    "    Console.WriteLine(acc);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The CLASS-OWN type-parameter twin: `T` is declared by the ENCLOSING TYPE
// rather than by the method, and the instantiation is written at the call
// site (`G<() -> int>.AddTo(arr, f)`). The element category is fixed at the
// instantiation either way (§3.6 rule 2 / §14.1), so the slot takes the
// element's own reference in both spellings and the caller's own reference
// must be dropped in both.
//
// RED on the base for the same reason (12800 B / 200 allocations).
const char* kClassOwnGenericAddToFunctionElement =
    "module App;\n"
    "import System.Collections;\n"
    "public class G<T>\n"
    "  public static method AddTo(xs: Array<T>, x: T)\n"
    "    xs.Add(x);\n"
    "  end AddTo;\n"
    "end G;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var acc := 0;\n"
    "    for var i in 0 ..< 200 do\n"
    "      using var arr: Array<() -> int> := new Array<() -> int>();\n"
    "      do\n"
    "        var f: () -> int := lambda() => 2;\n"
    "        G<() -> int>.AddTo(arr, f);\n"
    "      end;\n"
    "      acc := acc + arr[0]();\n"
    "    end for;\n"
    "    Console.WriteLine(acc);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// ── the two BASE-GREEN boundary controls (owner, 2026-11-12: at most two per
// suite, each named in the commit body with the wrong fix it catches) ──
//
// CONTROL 1 — the DIRECT spelling. The closure is stored into a concrete
// `Array<() -> int>` in the CALLER's own body, so the store is the built-in
// `Array<Fn>` row the compiler already admitted (the #9104/#9112 relief).
// It was clean on the base and must stay clean: it catches the plausible
// wrong fix "arm the caller's release for every store of a function value"
// (the direct spelling's release would then be armed twice — once by the
// built-in relief, once by the new one — and the closure's cell would be
// freed while the collection still held it: a use-after-free).
const char* kDirectAddFunctionElement =
    "module App;\n"
    "import System.Collections;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var acc := 0;\n"
    "    for var i in 0 ..< 200 do\n"
    "      using var arr: Array<() -> int> := new Array<() -> int>();\n"
    "      do\n"
    "        var f: () -> int := lambda() => 2;\n"
    "        arr.Add(f);\n"
    "      end;\n"
    "      acc := acc + arr[0]();\n"
    "    end for;\n"
    "    Console.WriteLine(acc);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL 2 — the SAME generic helper at a NON-function type parameter
// (`T := string`). The §28.11 element-slot row for a `string` element is the
// same shared-reference row, but the string reference is a different runtime
// object, and the generic body's store must keep taking exactly ONE
// reference there too. It was clean on the base and must stay clean: it
// catches the plausible wrong fix "resolve the element type of a generic
// collection to a function type unconditionally" (the string element would
// then be released by the CLOSURE release thunk — a mismatched release, i.e.
// a wild free).
const char* kGenericAddToStringElement =
    "module App;\n"
    "import System.Collections;\n"
    "public class G\n"
    "  public static method AddTo<T>(xs: Array<T>, x: T)\n"
    "    xs.Add(x);\n"
    "  end AddTo;\n"
    "end G;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var acc := 0;\n"
    "    for var i in 0 ..< 200 do\n"
    "      using var arr: Array<string> := new Array<string>();\n"
    "      do\n"
    "        var s: string := \"abc\";\n"
    "        G.AddTo(arr, s);\n"
    "      end;\n"
    "      acc := acc + arr[0].Length;\n"
    "    end for;\n"
    "    Console.WriteLine(acc);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// THE REGRESSION ROW: the issue's repro, verbatim, in a 200x loop.
//
// RED on r30-c1 @ d66898662: "SUMMARY: AddressSanitizer: 12800 byte(s) leaked
// in 200 allocation(s)." GREEN with the fix: prints 400, exits 0, leaks
// nothing.
BEZEL_TEST(generic_array_add_function_element_9320,
           generic_add_of_a_function_element_takes_its_reference_spec_28_11) {
    expectClean(runAotChecked(kGenericAddToFunctionElement), 0, "400\n");
}

// The CLASS-OWN type-parameter twin (the type parameter is declared by the
// enclosing type, and the instantiation is written at the call site).
//
// RED on the base for the same reason. GREEN with the fix.
BEZEL_TEST(generic_array_add_function_element_9320,
           class_own_generic_add_of_a_function_element_spec_28_11) {
    expectClean(runAotChecked(kClassOwnGenericAddToFunctionElement), 0, "400\n");
}

// CONTROL 1 — the DIRECT spelling, unchanged.
BEZEL_TEST(generic_array_add_function_element_9320,
           direct_add_of_a_function_element_unchanged_spec_28_11) {
    expectClean(runAotChecked(kDirectAddFunctionElement), 0, "400\n");
}

// CONTROL 2 — the same generic helper at `T := string`, unchanged.
BEZEL_TEST(generic_array_add_function_element_9320,
           generic_add_of_a_string_element_unchanged_spec_28_11) {
    expectClean(runAotChecked(kGenericAddToStringElement), 0, "600\n");
}
