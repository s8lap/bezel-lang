// Regression tests for issue #8871 — §28.3 closure bullet *Provenance*
// clause 6 and *Set-erasing positions* (v0.359).
//
// THE DEFECT.  `exprIsFunctionValueWithNonEmptyCaptureSet`
// (src/ownership/Ownership_Checks.cpp) decides, for a §28.3 set-erasing
// position, whether the function value reaching it has a NON-EMPTY source
// set.  For an identifier it read ONLY the local's lambda-literal
// INITIALIZER.  A local REASSIGNED a borrow-capturing lambda, and an ALIAS of
// a borrow-capturing local (whose initializer is not a lambda literal at
// all), therefore read as capture-free and were ADMITTED: the closure was
// stored through `Object`, outlived the borrow it captured, and read freed
// memory.
//
// THE SPEC.  §28.3 (v0.359), closure bullet, *Provenance*, clause 6:
//
//   "A **local or parameter** contributes, at each read, the union of the sets
//    of the forms the flow analysis traces it to, over every assignment that
//    reaches the read — a captured local or parameter over every assignment
//    to it in any body (clause 1)"
//
// clause 1, of a CAPTURED binding ("a local or parameter, in any state, that a
// lambda in the body captures"):
//
//   "holds ... every value it is ever assigned: its set is the union, over its
//    initializer and every assignment to it in any body ... whatever the
//    assignment's position relative to the capture or to the read"
//
// and *Set-erasing positions*:
//
//   "**At every set-erasing position a function value with a non-empty source
//    set is rejected, and a capture-free one is admitted**"
//
// THE ISSUE'S OWN REPROS (issue #8871; review lane r42-rev-s74's probes
// `v01d_reassign_freeinit_then_res.bzl` and `v02_alias_of_capturing_local.bzl`)
// are the first two rows, verbatim.  MEASURED on the base tree (r30-c1 @
// fbe542d4d): both compile; under `--sanitize=address` both report
// `heap-use-after-free`.
//
// ROWS.  Every row of `set_erasing_reaching_assignment_8871` is RED on the
// base tree: a REJECT row's program compiles there (and is a use-after-free),
// an ACCEPT row's program is rejected there.  The suite
// `set_erasing_reaching_assignment_controls_8871` holds the two named
// base-GREEN controls (RUN.md: at most two per suite); the other
// spec-decided controls live in this lane's `corpus-candidates/`.
//
// ORACLE.  `runAot` for the verdict rows; `runAotChecked` (the checked AOT
// path with the AddressSanitizer pass over the GENERATED module and
// LeakSanitizer at exit) for the admitted, looping row.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kCarriesBorrow = "function value carries captured borrow(s)";
const char* kAtObject = "may not be passed where the parameter is declared Object";
const char* kAtBareTypeParameter =
    "may not be passed where the parameter is declared at a bare type parameter";
const char* kAtExtensionReceiver =
    "may not be the receiver of an extension call whose head is `Object`";

// Rejected with the set-erasing diagnostic for the named position (not some
// unrelated compile error, and not a compiler crash).
void expectSetErasingReject(const JitResult& r, const char* position) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, position));
}

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

// The issue's harness, verbatim (issue #8871, Repro 1): `Reg2.Keep(o: Object)`
// stores the closure, and `RunInt()` calls it after the borrow source `r` has
// been disposed at the end of the inner `do` block.  `mk` is the whole `Mk`
// method; `Main` calls it 200 times.  `Keep<T>` and the `extension Object`
// give the other two set-erasing positions.
std::string registryProgram(const std::string& mk, const std::string& call) {
    return "module M; "
           "import System; "
           "public class Res "
           "  public V: int; "
           "  public constructor() V := 42; end constructor; "
           "  public method Get(): int => V; "
           "end Res; "
           "extension Object "
           "  public method Hold(): int => 1; "
           "end; "
           "public class Reg2 "
           "  private _o: Object? := null; "
           "  public constructor() end constructor; "
           "  public method Keep(o: Object) _o := o; end Keep; "
           "  public method RunInt(): int => (_o as () -> int)!(); "
           "end Reg2; "
           "public class Keep<T> "
           "  private _v: T? := null; "
           "  public constructor() end constructor; "
           "  public method Put(a: T) _v := a; end Put; "
           "end Keep; "
           "public class Program " +
           mk +
           "  public static method Main(): int "
           "    using var reg := new Reg2(); "
           "    for var i in 0 ..< 200 do "
           "      do "
           "        var r := new Res(); " +
           call +
           "      end; "
           "      Console.WriteLine(reg.RunInt()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// `Mk(reg, r)` with the given body.
std::string mk2(const std::string& body) {
    return "  public static method Mk(reg: Reg2, r: Res) " + body + "  end Mk; ";
}

// `Mk(reg, r, c)` with the given body.
std::string mk3(const std::string& body) {
    return "  public static method Mk(reg: Reg2, r: Res, c: bool) " + body +
           "  end Mk; ";
}

const char* kCall2 = "        Mk(reg, r); ";
const char* kCall3 = "        Mk(reg, r, i mod 2 = 0); ";

}  // namespace

// REJECT — issue #8871, Repro 1, verbatim (probe v01d): the initializer is
// capture-free, the REASSIGNMENT captures the borrowed parameter `r`, and the
// reassignment reaches the read at the `Object` argument (clause 6).  RED on
// the base tree: the program compiled there and read freed memory.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           issue_repro_1_capture_free_initializer_then_borrow_reassignment_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var f := lambda() => 0; "
            "    f := lambda() => r.V; "
            "    reg.Keep(f); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — issue #8871, Repro 2, verbatim (probe v02): an ALIAS.  `g`'s
// initializer is the read of `f`, which clause 6 traces to `f`'s lambda.
// RED on the base tree (compiled; printed freed memory).
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           issue_repro_2_alias_of_a_capturing_local_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var f := lambda() => r.V; "
            "    var g := f; "
            "    reg.Keep(g); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — the alias made by an ASSIGNMENT rather than an initializer: the
// kill of `g`'s capture-free initializer does not make `g` capture-free, the
// assigned value's set is `f`'s.  RED on the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           alias_by_assignment_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var f := lambda() => r.V; "
            "    var g := lambda() => 0; "
            "    g := f; "
            "    reg.Keep(g); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — the reassignment sits in ONE arm of an `if` with no `else`: it
// reaches the read after the `if` on the path through that arm, so the read
// is the union of the initializer and the reassignment (clause 6: "over every
// assignment that reaches the read").  RED on the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           reassignment_in_one_if_arm_reaches_the_read_rejected) {
    auto r = runAot(registryProgram(
        mk3("    var f := lambda() => 5; "
            "    if c then "
            "      f := lambda() => r.V; "
            "    end if; "
            "    reg.Keep(f); "),
        kCall3));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — a LOOP-CARRIED reassignment: the assignment follows the read in
// the text and reaches it on the loop's back edge (second iteration).  RED on
// the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           loop_carried_reassignment_reaches_the_read_on_the_back_edge_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var f := lambda() => 0; "
            "    for var j in 0 ..< 2 do "
            "      reg.Keep(f); "
            "      f := lambda() => r.V; "
            "    end for; "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — the assignment is in a LAMBDA BODY, so `f` is a captured binding:
// it holds "every value it is ever assigned ... whatever the assignment's
// position relative to the capture or to the read" (clause 1; clause 6 "over
// every assignment to it in any body").  RED on the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           assignment_in_a_lambda_body_to_a_captured_local_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var f := lambda() => 0; "
            "    var st := lambda() => do f := lambda() => r.V; end; "
            "    st(); "
            "    reg.Keep(f); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — a reassigned function-typed PARAMETER ("a local or parameter",
// clause 6): the assignment kills the value the caller passed and the read
// carries the assigned lambda's set.  RED on the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           reassigned_function_typed_parameter_rejected) {
    auto r = runAot(registryProgram(
        "  public static method Mk(reg: Reg2, r: Res, q: () -> int) "
        "    q := lambda() => r.V; "
        "    reg.Keep(q); "
        "  end Mk; ",
        "        Mk(reg, r, lambda() => 0); "));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — a `const` binding of a borrow-capturing lambda: a local constant
// is a local, its one definition is its value.  RED on the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           const_bound_lambda_rejected) {
    auto r = runAot(registryProgram(
        mk2("    const g := lambda() => r.V; "
            "    reg.Keep(g); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — a local bound to an INSTANCE METHOD REFERENCE: the form the local
// is traced to captures its receiver (§28.3 *Provenance* clause 2, v0.359).
// RED on the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           method_reference_bound_to_a_local_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var m := r.Get; "
            "    reg.Keep(m); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — an alias through `??`: either operand may be the value.  RED on
// the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           null_coalescing_alias_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var f := lambda() => r.V; "
            "    var n: (() -> int)? := null; "
            "    var g := n ?? f; "
            "    reg.Keep(g); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — the reassigned local at the second set-erasing argument position,
// a BARE TYPE PARAMETER (`Keep<Object>.Put(a: T)`).  RED on the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           reassigned_local_at_bare_type_parameter_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var f := lambda() => 0; "
            "    f := lambda() => r.V; "
            "    var k := new Keep<Object>(); "
            "    k.Put(f); "
            "    reg.Keep(lambda() => 1); "),
        kCall2));
    expectSetErasingReject(r, kAtBareTypeParameter);
}

// REJECT — the alias as the RECEIVER of an extension call whose head is
// `Object` (§28.3 *Set-erasing positions*, clause 4).  RED on the base tree.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           alias_at_extension_object_receiver_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var f := lambda() => r.V; "
            "    var g := f; "
            "    var z := g.Hold(); "
            "    reg.Keep(lambda() => 1); "),
        kCall2));
    expectSetErasingReject(r, kAtExtensionReceiver);
}

// ACCEPT — the borrow-capturing initializer is OVERWRITTEN, on the only path
// to the read, by a capture-free lambda: the initializer does not reach the
// read (clause 6: "over every assignment that reaches the read"), the set is
// empty, and the position admits it.  The program loops 200 times under the
// checked oracle: it must print the sum and leak nothing.  RED on the base
// tree: the program was REJECTED there (the initializer alone was read).
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           borrow_initializer_overwritten_by_capture_free_lambda_accepted) {
    auto r = runAotChecked(
        std::string("module M; "
                    "import System; "
                    "public class Res "
                    "  public V: int; "
                    "  public constructor() V := 42; end constructor; "
                    "end Res; "
                    "public class Program "
                    "  public static method Take(o: Object) end Take; "
                    "  public static method Main(): int "
                    "    var total := 0; "
                    "    for var i in 0 ..< 200 do "
                    "      var r := new Res(); "
                    "      var f := lambda() => r.V; "
                    "      f := lambda() => 7; "
                    "      Take(f); "
                    "      total := total + f(); "
                    "    end for; "
                    "    Console.WriteLine(total); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1400\n");
    assertNoLeak(r);
}

// ACCEPT — #8785's rule for a REASSIGNED local: both the initializer and the
// reassignment capture only a value-type local, every reaching assignment
// contributes nothing (§28.3 *Provenance* clause 1), so the union is empty.
// RED on the base tree (rejected there), and still rejected by #8785's own
// fix, which keeps the pre-#8785 count for any reassigned local.
BEZEL_TEST(set_erasing_reaching_assignment_8871,
           value_capture_reassigned_to_value_capture_accepted) {
    auto r = runAotChecked(
        std::string("module M; "
                    "import System; "
                    "public class Program "
                    "  public static method Take(o: Object) end Take; "
                    "  public static method Main(): int "
                    "    var total := 0; "
                    "    for var i in 0 ..< 200 do "
                    "      var n := i; "
                    "      var m := i + 1; "
                    "      var f := lambda() => n; "
                    "      f := lambda() => m; "
                    "      Take(f); "
                    "      total := total + f(); "
                    "    end for; "
                    "    Console.WriteLine(total); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "20100\n");
    assertNoLeak(r);
}

// ---------------------------------------------------------------------------
// The two named controls.  Each program is REJECTED on the base tree too
// (base-GREEN), and each is RED under a named wrong fix.

// CONTROL k15 — a CAPTURED binding is not killed by a later assignment.  `f`
// is assigned in `st`'s body, so it is a captured binding and holds "every
// value it is ever assigned ... whatever the assignment's position relative
// to the capture or to the read" (clause 1): the `f := lambda() => n + 1`
// that textually precedes the read does NOT kill the assignment `st()`
// performs after it.  WRONG FIX this catches: a reaching-assignment trace
// that applies its kill rule to a captured binding — it reads only
// `lambda() => n + 1`, admits the program, and `RunInt` then reads the
// disposed `r` (the rejected 5ec864693 admitted it too, reading only the
// initializer).
BEZEL_TEST(set_erasing_reaching_assignment_controls_8871,
           k15_captured_binding_is_not_killed_by_a_later_assignment_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var n := 3; "
            "    var f := lambda() => n; "
            "    var st := lambda() => do f := lambda() => r.V; end; "
            "    f := lambda() => n + 1; "
            "    st(); "
            "    reg.Keep(f); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}

// CONTROL w60 — the assignment to `f` is in a lambda body inside a BINDING
// ATOM (`(var t := 1; lambda() => do f := … end)`).  WRONG FIX this catches:
// taking "is this local reassigned?" from the ownership pre-scan's
// `scopedAssignedSyms_`, which does not descend into a binding atom — `f`
// then reads as never reassigned, its value-capturing initializer as its only
// definition, and the program is admitted (MEASURED on that variant of the
// fix: ADMIT).  The trace walks the body itself, binding atoms included.
BEZEL_TEST(set_erasing_reaching_assignment_controls_8871,
           w60_assignment_in_a_binding_atom_lambda_rejected) {
    auto r = runAot(registryProgram(
        mk2("    var n := 1; "
            "    var f := lambda() => n; "
            "    var z := (var t := 1; lambda() => do f := lambda() => r.V; return 5; end); "
            "    var u := z(); "
            "    reg.Keep(f); "),
        kCall2));
    expectSetErasingReject(r, kAtObject);
}
