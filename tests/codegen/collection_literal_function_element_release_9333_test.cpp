// Codegen — issue #9333: a COLLECTION LITERAL of function-typed locals leaked
// every element local.
//
// THE RULE. §28.11 (v0.359) puts a function type in the shared-reference row
// of the collection element-slot table: a built-in collection's STORE takes a
// reference of its OWN ("the store: stores the reference") and gives that
// reference back when the slot goes away. §28.9 (v0.359): a function-typed
// value is runtime-managed. So the local the element was read from keeps its
// OWN reference and releases it at its scope end — exactly as for a direct
// `arr.Add(f)` (issue #9104, the `storedIntoSharedRefSlot_` relief).
//
// THE DEFECT. The literal materializer does the TAKE half correctly: it emits
// a `bz_closure_retain` per element before `bz_array_add`, so the slot holds
// a reference of its own (MEASURED in the IR, `BEZEL_DUMP_IR=1`). But the
// closure escape walker's COLLECTION-LITERAL arm
// (`ClosureEscapeWalker`, `src/codegen/LlvmBackend_Setup.cpp`, reached from
// `Emitter::collectClosureReleaseEligible`) read each element as a plain
// ESCAPE — it did not mark the read as a reference-taking site the way the
// `Add` / `Insert` / `Set` argument arm does
// (`isBuiltinArrayFunctionElementStore` sets `inReferenceTakingSite_` and
// `storedIntoSharedRefSlot_`). The element locals therefore never got their
// scope-end release armed, and their references were stranded: the collection
// held one reference that was never given back and the local held another
// that was never dropped.
//
// MEASURED on the base (r30-c1 @ d6689866287ec03d095eac64589b741460339173,
// `bezelc --sanitize=address`, 200 iterations):
//
//   using var st: Array<() -> int> := [f, g];
//     -> "SUMMARY: AddressSanitizer: 28000 byte(s) leaked in 800 allocation(s)."
//        (the issue's own repro: the two closure cells, `g`'s environment and
//        `g`'s cell)
//   const st: Array<() -> int> := [f, g];
//     -> the same 28000 B / 800 allocations
//   return [f, g];  (a method whose return type is `owned Array<() -> int>`)
//     -> the same 28000 B / 800 allocations
//
// With the fix all three print 20100 and leak NOTHING.
//
// FAIL-WITHOUT / PASS-WITH: every row below runs under the ASan + LSan AOT
// runner (`runAotChecked`), so `ran == false` IS the leak assertion — a row
// whose program leaks is RED, not green-with-a-warning. Every program loops
// 200 times so a one-shot leak can never hide under the sanitizer's noise, and
// every row asserts the program's own value (a row whose body is a constant
// cannot see a fix that misreads the data it handles).
//
// THE TWO CONTROLS are green on the base and on the fix; they are here to catch
// the two plausible WRONG fixes, not to pin the leak:
//   * CONTROL 1 (fresh lambdas in the literal): catches the wrong fix "treat
//     every element of the literal as a retained value" — a fresh lambda's
//     reference is TRANSFERRED into the slot (the producer has no other
//     holder), so a second release would free a closure the slot still holds
//     (a use-after-free / double free).
//   * CONTROL 2 (the `arr.Add(f)` twin): catches the wrong fix "change the
//     store arm" — the issue states the direct store is already correct, so a
//     fix that touched it would be a behaviour change, not a fix.

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

}  // namespace

// ── THE REGRESSION ROWS ───────────────────────────────────────────────────
//
// The issue's repro, verbatim, in a 200x loop. `f` and `g` are closure
// locals; `st` is an `Array<() -> int>` whose slot takes a reference of its
// own, so `f` and `g` keep their own references and release them at the
// scope end.
//
// RED on the base: 28000 B leaked in 800 allocations (200 iterations).
// GREEN with the fix: prints 20100, exits 0, leaks nothing.
BEZEL_TEST(collection_literal_function_element_release_9333,
           using_var_array_literal_local_spec_28_11) {
    expectClean(runAotChecked(R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      var k := i;
      var f: () -> int := lambda() => 1;
      var g: () -> int := lambda() => k;
      using var st: Array<() -> int> := [f, g];
      acc := acc + st[0]() + st[1]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL"),
        0, "20100\n");
}

// The `const` twin of the row above: the same literal bound to a `const`
// local. §21.1's `const` local owns its value, so the element locals still
// keep their own references and release them at the scope end.
//
// RED on the base: the same 28000 B / 800 allocations.
// GREEN with the fix: prints 20100, exits 0, leaks nothing.
BEZEL_TEST(collection_literal_function_element_release_9333,
           const_array_literal_local_spec_28_11) {
    expectClean(runAotChecked(R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      var k := i;
      var f: () -> int := lambda() => 1;
      var g: () -> int := lambda() => k;
      const st: Array<() -> int> := [f, g];
      acc := acc + st[0]() + st[1]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL"),
        0, "20100\n");
}

// The RETURN position: the literal is stored into the method's RETURN slot
// (`owned Array<() -> int>`), which takes a reference of its own exactly as
// the local's slot does. The element locals go out of scope when `Make`
// returns, so their references must be released there.
//
// RED on the base: the same 28000 B / 800 allocations.
// GREEN with the fix: prints 20100, exits 0, leaks nothing.
BEZEL_TEST(collection_literal_function_element_release_9333,
           returned_array_literal_spec_28_11) {
    expectClean(runAotChecked(R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Make(k: int): owned Array<() -> int>
    var f: () -> int := lambda() => 1;
    var g: () -> int := lambda() => k;
    return [f, g];
  end Make;
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      var arr := Make(i);
      acc := acc + arr[0]() + arr[1]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL"),
        0, "20100\n");
}

// ── the two BASE-GREEN boundary controls (owner, 2026-11-12: at most two per
// suite, each named in the commit body with the wrong fix it catches) ──
//
// CONTROL 1 — the literal of FRESH lambdas. A fresh lambda's reference is
// TRANSFERRED into the slot (`emitFunctionElementSlotRetain`'s producer
// arm), so the slot holds the only reference and the literal's free gives it
// back exactly once. It was clean on the base. It catches the plausible
// wrong fix "release every element of the literal at the scope end" (the
// transferred reference would be released twice — a use-after-free in the
// slot's own read).
BEZEL_TEST(collection_literal_function_element_release_9333,
           fresh_lambda_literal_no_double_free_spec_28_11) {
    expectClean(runAotChecked(R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      using var st: Array<() -> int> := [lambda() => 1, lambda() => 2];
      acc := acc + st[0]() + st[1]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL"),
        0, "600\n");
}

// CONTROL 2 — the `arr.Add(f)` twin, the shape the issue names as ALREADY
// CORRECT (`storedIntoSharedRefSlot_`). It was clean on the base. It catches
// the plausible wrong fix "change the store arm too" — the direct store
// already takes a reference of its own and already gives it back, so a fix
// that touched it would change a correct behaviour.
BEZEL_TEST(collection_literal_function_element_release_9333,
           array_add_store_twin_spec_28_11) {
    expectClean(runAotChecked(R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      var k := i;
      var f: () -> int := lambda() => 1;
      var g: () -> int := lambda() => k;
      using var st: Array<() -> int> := new Array<() -> int>();
      st.Add(f);
      st.Add(g);
      acc := acc + st[0]() + st[1]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL"),
        0, "20100\n");
}
