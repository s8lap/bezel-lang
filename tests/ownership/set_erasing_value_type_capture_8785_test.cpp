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
// The two named boundary controls (RUN.md, owner 2026-11-12: at most two per
// suite).  "Contributes nothing" is reached only for a capture POSITIVELY
// identified as a value type that holds no borrow; both programs below
// capture a value that HOLDS one, so §28.3 (v0.359), closure bullet,
// *Provenance*, clause 1 counts it:
//
//   "a captured local or parameter of a value type that holds borrows
//    ([§3.6]) the sources of the borrows it carries (in a generic body, read
//    at each instantiation at which the type, after substitution, holds
//    borrows, ...)"
//
// and *Set-erasing positions*: "a function value with a non-empty source set
// is rejected".  Each program is REJECTED on the base tree too (it is a
// base-GREEN control, not a fix row), and each is RED under a named wrong
// fix: the rejected fix-forward a28e5cb98 ADMITTED both, and under ASan over
// these 200 iterations each then reported `heap-use-after-free` (review
// r42-rev-s65, witnesses u01 and u03).  The other must-still-reject shapes
// (stdlib-class, span, owned-local and borrow-bearing-record captures) are
// base-green and live in the conformance corpus.

namespace {

const char* kCarriesBorrow = "function value carries captured borrow(s)";
const char* kAtObject = "may not be passed where the parameter is declared Object";

// Rejected at the `Object` argument with the set-erasing diagnostic (not
// some unrelated compile error).
void expectSetErasingRejectAtObject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// The run-time witness harness: `Reg2.Keep(o: Object)` stores the closure and
// `RunInt()` calls it after the captured value's borrow source `r` has been
// disposed at the end of the inner `do` block.
std::string registryProgram(const std::string& decls,
                            const std::string& members,
                            const std::string& storeCall) {
    return "module M; "
           "import System; "
           "public class Res "
           "  public V: int; "
           "  public constructor() V := 42; end constructor; "
           "end Res; "
           "public class Reg2 "
           "  private _o: Object? := null; "
           "  public constructor() end constructor; "
           "  public method Keep(o: Object) _o := o; end Keep; "
           "  public method RunInt(): int => (_o as () -> int)!(); "
           "end Reg2; " +
           decls +
           " public class Program " + members +
           "  public static method Main(): int "
           "    using var reg := new Reg2(); "
           "    for var i in 0 ..< 200 do "
           "      do "
           "        var r := new Res(); " +
           storeCall +
           "      end; "
           "      Console.WriteLine(reg.RunInt()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// u01 (review r42-rev-s65): a `where T: value` type-parameter capture.  `where
// T: value` admits a borrow-bearing record (§9.7's generic-body paragraph:
// `Id<T>(a: T) where T: value` at `Id<Pair>` over `record Pair(Item: Item)`
// is rejected), and clause 1 reads the capture "at each instantiation at which
// the type, after substitution, holds borrows".  The body is checked once,
// with no per-instantiation re-check, so the capture stays COUNTED at every
// instantiation.  WRONG FIX this catches: a28e5cb98's top-level `where T:
// value` exemption, which admitted `G<HV>` and printed freed memory.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           u01_where_value_type_parameter_capture_at_object_rejected) {
    auto r = runAot(registryProgram(
        "public interface IGetV "
        "  method GetV(): int; "
        "end IGetV; "
        "record HV(It: Res): IGetV "
        "  public method GetV(): int => It.V; "
        "end HV;",
        "  public static method G<T>(reg: Reg2, x: T) where T: value, IGetV "
        "    reg.Keep(lambda() => x.GetV()); "
        "  end G; ",
        "        G<HV>(reg, new HV(r)); "));
    expectSetErasingRejectAtObject(r);
}

// u03 (review r42-rev-s65): a body record whose class reference is an
// AUTO-PROPERTY.  §9.7: "Reference-typed fields in records are always
// borrows"; an auto-property is a storage component of the record (§9.2), so
// `BR` holds a borrow and the capture of `h` contributes its source.  WRONG
// FIX this catches: a28e5cb98's record walk over `sema::anyRecordStorageField`,
// which skips auto-properties (the #6708 gap) and so read `BR` as holding
// none.
BEZEL_TEST(set_erasing_borrow_capture_8785,
           u03_body_record_auto_property_capture_at_object_rejected) {
    auto r = runAot(registryProgram(
        "record BR "
        "  public readonly property It: Res; "
        "  public constructor(it: Res) It := it; end constructor; "
        "end BR;",
        "  public static method F(reg: Reg2, h: BR) "
        "    reg.Keep(lambda() => h.It.V); "
        "  end F; ",
        "        F(reg, new BR(r)); "));
    expectSetErasingRejectAtObject(r);
}

// ---------------------------------------------------------------------------
// The REASSIGNED local (fix-forward; review r42-rev-s74 of 5ec864693).
//
// §28.3 (v0.359), closure bullet, *Provenance*, clause 6:
//
//   "A **local or parameter** contributes, at each read, the union of the sets
//    of the forms the flow analysis traces it to, over every assignment that
//    reaches the read"
//
// The value exemption of clause 1 is a statement about ONE lambda literal.  A
// local read at a set-erasing position is judged over every assignment that
// reaches the read, so a local whose INITIALIZER captures only a value and
// which is then reassigned a borrow-capturing lambda carries that borrow.
//
// The two rows below are the suite's two named controls (RUN.md: at most two
// per suite).  Each is REJECTED on the base tree r30-c1 (base-GREEN) and each
// is RED under the named wrong fix — the rejected intermediate commit
// 5ec864693, whose identifier arm exempted the value captures of the local's
// initializer lambda and looked at nothing else: it ADMITTED both, and under
// ASan each was a `heap-use-after-free` (review r42-rev-s74, witnesses v01c
// and v05).  The third witness of that review (v01, a `StringBuilder` in
// place of `Res`) is the same shape and lives in this lane's
// `corpus-candidates/`.

namespace {

const char* kAtBareTypeParameter =
    "may not be passed where the parameter is declared at a bare type parameter";

}  // namespace

// v01c (review r42-rev-s74): the initializer captures the value-type local
// `n`; the reassignment captures the borrowed parameter `r`; the read at the
// `Object` argument is reached by the reassignment.  WRONG FIX this catches:
// 5ec864693's initializer-only value exemption.
BEZEL_TEST(set_erasing_reassigned_local_8785,
           v01c_value_initializer_then_borrow_reassignment_at_object_rejected) {
    auto r = runAot(registryProgram(
        "",
        "  public static method Mk(reg: Reg2, r: Res) "
        "    var n := 1; "
        "    var f := lambda() => n; "
        "    f := lambda() => r.V; "
        "    reg.Keep(f); "
        "  end Mk; ",
        "        Mk(reg, r); "));
    expectSetErasingRejectAtObject(r);
}

// v05 (review r42-rev-s74): the same reassigned local at the OTHER argument
// position — a bare type parameter (`Keep<Object>.Put(a: T)`).  WRONG FIX
// this catches: 5ec864693's initializer-only value exemption (the predicate
// is shared by every set-erasing position, so a guard added at the `Object`
// argument alone would leave this one admitted).
BEZEL_TEST(set_erasing_reassigned_local_8785,
           v05_value_initializer_then_borrow_reassignment_at_bare_type_parameter_rejected) {
    auto r = runAot(registryProgram(
        "public class Keep<T> "
        "  private _v: T? := null; "
        "  public constructor() end constructor; "
        "  public method Put(a: T) _v := a; end Put; "
        "end Keep; ",
        "  public static method Mk(r: Res) "
        "    var n := 1; "
        "    var k := new Keep<Object>(); "
        "    var f := lambda() => n; "
        "    f := lambda() => r.V; "
        "    k.Put(f); "
        "  end Mk; ",
        "        Mk(r); "
        "        reg.Keep(lambda() => 1); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kAtBareTypeParameter));
}
