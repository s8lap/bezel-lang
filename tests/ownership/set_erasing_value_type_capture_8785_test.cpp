// Regression tests for issue #8785 — §28.3 closure bullet *Provenance*
// clause 1 and *Set-erasing positions* (v0.359).
//
// THE DEFECT.  `exprIsFunctionValueWithNonEmptyCaptureSet`
// (src/ownership/Ownership_Checks.cpp) decides, for a §28.3 set-erasing
// position, whether the function value reaching it has a NON-EMPTY source
// set.  It counted EVERY capture of a lambda, so a lambda capturing only a
// VALUE-TYPE local (`var n := 5;`) was treated as capture-bearing and
// REJECTED at every set-erasing position: an `Object` argument, a bare type
// parameter substituted with a function type, and — since #7259 — the
// receiver of an extension call whose head is `Object`.
//
// THE SPEC.  §28.3 (v0.359), closure bullet, *Provenance*, clause 1:
//
//   "A **lambda literal** contributes the sources of the borrows it captures
//    ... a captured local or parameter of a value type that holds borrows
//    ([§3.6]) the sources of the borrows it carries, ...; **a captured value
//    that carries no borrow — a value type that holds none, a null-source
//    value, a `string` — contributes nothing**, and lives in its heap capture
//    cell)"
//
// and §28.3 (v0.359), *Set-erasing positions*:
//
//   "**At every set-erasing position a function value with a non-empty source
//    set is rejected, and a capture-free one is admitted**"
//
// A value-type capture carries no borrow, so it contributes nothing to the
// source set: the set stays EMPTY, the closure is capture-free, and the
// position must ADMIT it.  The predicate now counts a capture only when it
// contributes a source (the new `captureContributesSource`).
//
// THE ISSUE'S OWN REPRO (issue #8785; the review lane r42-rev-m63's probe
// `/var/tmp/bezel-scratch/r42-rev-m63/probes/b_value_capture.bzl`):
//
//   var n := 5;
//   var f: () -> int := lambda() => n;
//   Console.WriteLine(f.Hold());          // `Hold` is `extension Object`
//
// MEASURED on the base tree (r30-c1 @ 80439bb9d): that program is REJECTED
// at compile time.  With the fix it compiles, prints 1 and leaks nothing.
//
// The rows below are RED on the base tree (the programs do not compile there)
// and GREEN with the fix.  The must-still-reject shapes (a class-typed
// capture, a borrow-bearing record capture, a function-typed capture) are
// base-GREEN controls and therefore live in the conformance corpus, not here
// — see this lane's `corpus-candidates/`.
//
// ORACLE.  `runAot` for the verdict rows; `runAotChecked` (the checked AOT
// path with the AddressSanitizer pass over the GENERATED module and
// LeakSanitizer at exit) for the leak row, which LOOPS 200 iterations so a
// leak cannot hide under the sanitizer's noise.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes.  LSan
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

// The issue's own repro, verbatim (issue #8785; the review lane r42-rev-m63's
// probe `b_value_capture.bzl`): a lambda capturing only the value-type local
// `n`, used as the RECEIVER of an extension call whose head is `Object`
// (§28.3 *Set-erasing positions*, clause 4).
const char* kIssueRepro =
    "module M; "
    "import System; "
    "class Res "
    "  public constructor() end constructor; "
    "  public method Use(): int return 3; end Use; "
    "  public method Add(): int return 4; end Add; "
    "  public static method Stat(): int return 5; end Stat; "
    "end Res; "
    "extension Object "
    "  public method Hold(): int => 1; "
    "end; "
    "public class Program "
    "  public static method Main(): int "
    "    var n := 5; "
    "    var f: () -> int := lambda() => n; "
    "    Console.WriteLine(f.Hold()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

// ACCEPT: the issue's own repro — a value-type capture contributes nothing to
// the source set (§28.3 *Provenance* clause 1), so the closure is capture-free
// and the extension-Object receiver — a set-erasing position (§28.3 clause 4)
// — admits it.  RED on the base tree: the program was REJECTED there.
BEZEL_TEST(set_erasing_value_type_capture_8785,
           value_type_capture_at_extension_object_receiver_accepted) {
    auto r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ACCEPT: the same capture at an `Object` ARGUMENT — the other set-erasing
// position (§28.3 *Set-erasing positions*: "a position declared `Object`").
// RED on the base tree.
BEZEL_TEST(set_erasing_value_type_capture_8785,
           value_type_capture_at_object_argument_accepted) {
    auto r = runAot(
        std::string("module M; "
                    "import System; "
                    "public class Program "
                    "  public static method Take(o: Object) end Take; "
                    "  public static method Main(): int "
                    "    var n := 5; "
                    "    var f: () -> int := lambda() => n; "
                    "    Take(f); "
                    "    Console.WriteLine(\"ok\"); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// ACCEPT: the same capture at a BARE TYPE PARAMETER substituted with a
// function type — the third set-erasing position (§28.3 *Set-erasing
// positions*: "a bare type parameter ... at an instantiation substituting a
// function type or `Object`").  RED on the base tree.
BEZEL_TEST(set_erasing_value_type_capture_8785,
           value_type_capture_at_bare_type_parameter_accepted) {
    auto r = runAot(
        std::string("module M; "
                    "import System; "
                    "public class Util "
                    "  public static method Take<T>(x: T) "
                    "    Console.WriteLine(\"took\"); "
                    "  end Take; "
                    "end Util; "
                    "public class Program "
                    "  public static method Main(): int "
                    "    var n := 5; "
                    "    var f: () -> int := lambda() => n; "
                    "    Util.Take(f); "
                    "    Console.WriteLine(\"ok\"); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "took\nok\n");
}

// The newly admitted program must not leak: the closure's capture cell holds
// the value-type local, and the cell is reclaimed when the closure dies.  The
// program LOOPS 200 times (the issue's shape, in a loop) so a per-iteration
// leak cannot hide under the sanitizer's noise.  RED on the base tree: the
// program does not compile there.
BEZEL_TEST(set_erasing_value_type_capture_8785,
           value_type_capture_is_leak_free_over_200_iterations) {
    auto r = runAotChecked(
        std::string("module M; "
                    "import System; "
                    "public class Program "
                    "  public static method Take(o: Object) end Take; "
                    "  public static method Main(): int "
                    "    var total := 0; "
                    "    for var i in 0..<200 do "
                    "      var n := i; "
                    "      var f: () -> int := lambda() => n; "
                    "      Take(f); "
                    "      total := total + f(); "
                    "    end; "
                    "    Console.WriteLine(total); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "19900\n");
    assertNoLeak(r);
}

// ---------------------------------------------------------------------------
// FIX-FORWARD (review r42-rev-m75 REJECTED 687462d48): only a POSITIVELY
// no-borrow value-type capture contributes nothing.
//
// The first cut of `captureContributesSource` defaulted to "contributes
// nothing" at every declaration-less type head, so a capture of a stdlib
// CLASS (`StringBuilder`, `Array<T>`, `Dictionary<K,V>`, `Random`) or of a
// span — and a record whose field is one — read as a value that holds no
// borrow, and the three set-erasing positions newly ADMITTED it.  §28.3
// (v0.359), closure bullet, *Provenance*, clause 1 counts every one of them:
//
//   "a captured borrowed local or parameter that binding's source; a
//    captured owned local its enclosing scope ...; a captured local or
//    parameter of a value type that holds borrows ([§3.6]) the sources of the
//    borrows it carries ...; a captured value that carries no borrow — a
//    value type that holds none, a null-source value, a `string` —
//    contributes nothing"
//
// and §9.7 makes a field borrow-carrying when "Its type is a reference type
// (class or interface, nullable or not ...)"; `Span<T>`/`ArraySpan<T>` are
// borrow-bearing records.  "Contributes nothing" is the exemption for a value
// type that holds NONE; nothing else reaches it.
//
// Every row below is the review's probe (`/var/tmp/bezel-scratch/
// r42-rev-m63/m75/probes/<id>.bzl`) and is RED at 687462d48 (the program
// compiled there) and GREEN with the fix-forward (rejected at the set-erasing
// position).  The base before 687462d48 rejected each of them too; these
// rows pin the inversion the first cut introduced.  u03 is the run-time
// witness: at 687462d48 it printed `0` (the disposed StringBuilder) instead
// of `6`, and leaked under ASan.

namespace {

// The review's shared probe prelude, trimmed to what the rows use.
std::string borrowCaptureProgram(const std::string& decls,
                                 const std::string& members) {
    return "module M; "
           "import System; "
           "import System.Text; "
           "public class Res "
           "  public constructor() end constructor; "
           "  public method Use(): int return 3; end Use; "
           "end Res; "
           "extension Object "
           "  public method Hold(): int => 1; "
           "end; " +
           decls +
           " public class Program "
           "  public static method Take(o: Object) end Take; " +
           members + " end Program; ";
}

const char* kCarriesBorrow = "function value carries captured borrow(s)";

// Rejected at a §28.3 set-erasing position, with the set-erasing diagnostic
// (not some unrelated compile error).
void expectSetErasingReject(const JitResult& r, const char* position) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, position));
}

const char* kAtObject = "may not be passed where the parameter is declared Object";

}  // namespace

// r04: a borrowed `StringBuilder` parameter — "a captured borrowed ...
// parameter that binding's source".
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r04_stringbuilder_param_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method F(sb: StringBuilder) Take(lambda() => sb.Length); end F; "
        "public static method Main(): int var sb := new StringBuilder(); F(sb); "
        "Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r05: a borrowed `Array<int>` parameter (a trivial-dispose CLASS).
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r05_array_int_param_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method F(a: Array<int>) Take(lambda() => a.Count); end F; "
        "public static method Main(): int var a := new Array<int>(3); F(a); "
        "Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r15: a span local — "a captured local ... of a value type that holds
// borrows ... the sources of the borrows it carries".
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r15_span_local_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method F(a: Array<int>) var s := a.AsSpan(); "
        "Take(lambda() => s.Length); end F; "
        "public static method Main(): int var a := new Array<int>(3); F(a); "
        "Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r16: an owned plain-`var` `StringBuilder` local — "a captured owned local
// its enclosing scope".
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r16_owned_stringbuilder_local_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method Main(): int var sb := new StringBuilder(); "
        "Take(lambda() => sb.Length); Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r17: a borrowed `Array<Res>` parameter.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r17_array_of_class_param_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method F(a: Array<Res>) Take(lambda() => a.Count); end F; "
        "public static method Main(): int var a := new Array<Res>(0); F(a); "
        "Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r20: the `StringBuilder` capture at the extension-`Object` RECEIVER
// position.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r20_stringbuilder_capture_at_extension_object_receiver_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method F(sb: StringBuilder): int "
        "var f: () -> int := lambda() => sb.Length; return f.Hold(); end F; "
        "public static method Main(): int var sb := new StringBuilder(); "
        "Console.WriteLine(F(sb)); return 0; end Main;"));
    expectSetErasingReject(r, "receiver of an extension call");
}

// r21: the `StringBuilder` capture at a BARE TYPE PARAMETER substituted with
// `Object` (`Keep<Object>.Put`).
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r21_stringbuilder_capture_at_bare_type_parameter_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "public class Keep<T> "
        "  public constructor() end constructor; "
        "  public method Put(a: T) end Put; "
        "end Keep;",
        "public static method F(sb: StringBuilder) using var k := new Keep<Object>(); "
        "k.Put(lambda() => sb.Length); end F; "
        "public static method Main(): int var sb := new StringBuilder(); F(sb); "
        "Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, "bare type parameter");
}

// r22: a borrowed `Dictionary<string, int>` parameter.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r22_dictionary_param_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method F(d: Dictionary<string, int>) Take(lambda() => d.Count); end F; "
        "public static method Main(): int var d := new Dictionary<string, int>(); F(d); "
        "Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r27: an owned local of an OWNING choice (auto-dropped) — "a captured owned
// local its enclosing scope".
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r27_owning_choice_local_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "choice Slot "
        "  Full(owned c: Res), "
        "  Empty; "
        "end Slot;",
        "public static method Main(): int var s := Slot.Full(new Res()); "
        "Take(lambda() => (case s of Full(_) => 1, _ => 0)); "
        "Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r28: an owned `[1 owned]Res` inline-array local.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r28_owning_inline_array_local_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method Main(): int var arr: [1 owned]Res := [new Res()]; "
        "Take(lambda() => arr[0].Use()); Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r32: an owned plain-`var` `Array<int>` local.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r32_owned_array_local_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "",
        "public static method Main(): int var a := new Array<int>(2); "
        "Take(lambda() => a.Count); Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// r35: a record whose field is a `StringBuilder` — a value type that HOLDS a
// borrow (§9.7 first bullet), read through clause 1's holds-borrows arm.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           r35_record_holding_stringbuilder_capture_at_object_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "record SBH(B: StringBuilder);",
        "public static method F(sb: StringBuilder) var h := new SBH(sb); "
        "Take(lambda() => h.B.Length); end F; "
        "public static method Main(): int var sb := new StringBuilder(); F(sb); "
        "Console.WriteLine(\"ran\"); return 0; end Main;"));
    expectSetErasingReject(r, kAtObject);
}

// u03: the run-time witness.  `Registry.Keep(o: Object)` stores the closure
// and `Run()` calls it after the captured `StringBuilder`'s scope has ended.
// At 687462d48 this compiled and printed `0` 200 times (the disposed
// builder) instead of `6`, with an ASan-reported leak; it is rejected at the
// `Object` argument with the fix.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           u03_registry_keep_of_scoped_stringbuilder_capture_rejected) {
    auto r = runAot(borrowCaptureProgram(
        "public class Registry "
        "  private _o: Object? := null; "
        "  public constructor() end constructor; "
        "  public method Keep(o: Object) _o := o; end Keep; "
        "  public method Run(): int => (_o as () -> int)!(); "
        "end Registry;",
        "public static method Main(): int "
        "  using var reg := new Registry(); "
        "  for var i in 0 ..< 200 do "
        "    do "
        "      var sb := new StringBuilder(\"abcdef\"); "
        "      reg.Keep(lambda() => sb.Length); "
        "    end; "
        "    Console.WriteLine(reg.Run()); "
        "  end for; "
        "  return 0; "
        "end Main;"));
    expectSetErasingReject(r, kAtObject);
}
