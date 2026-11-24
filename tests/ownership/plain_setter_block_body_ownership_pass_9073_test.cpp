// fix #9073 (§28.3.10 *Owned-local discharge requirement*, §28.3 *Borrow
// lifetime rules*, §28.9, §23.3, v0.359): a PLAIN property setter's BLOCK
// body runs no ownership pass, so its owned local is never disposed (a leak)
// and §28.3's borrowed-field-store rule is not applied there.
//
// THE DEFECT.  `ld->autoDropAtScopeEnd` — the flag codegen's plain-`var`
// scope-end disposal is gated on — is armed by the method-body ownership
// dataflow's scope-exit sweep (`MethodOwnership::run`).  A plain setter's
// block body (`set => do … end;`) reached NO ownership pass at all: #5252
// scoped its fix to the `set(owned)` spelling (`runOwnedSetterBody`), and
// #8767 scoped its fix to a PROVED-CONFINED block getter body
// (`runGetterBody`).  So the setter body's owned locals got
//   * no §28.3.10 discharge obligation — the destructor never ran and the
//     allocation leaked on every setter call, and
//   * no §28.3 verdict — a body that stores its scope-bound local into a
//     field was ADMITTED, where the byte-identical METHOD-body twin is
//     rejected.
//
// MEASURED on the base (r30-c1 @ 1e7e7b586), the issue's own repro verbatim:
// the program compiles with NO diagnostic at all, prints NO destructor output,
// and LeakSanitizer reports "Direct leak of 3200 byte(s) in 200 object(s)" —
// 16 B per setter call.  The same `var r := new Res();` in a METHOD body
// disposes `r` and prints the destructor; so do the `set(owned)` accessor twin
// and a confined block getter twin.
//
// THE SPEC.  §28.3.10 (v0.359): "When an owned local — or an `owned`
// parameter — reaches a *normal-exit* scope end without having reached a
// discharge point, and is not in moved-from state, it is disposed implicitly at
// that scope end" … "On a path that never moves it out, a plain `var` owned
// local … thus behaves like a `using var`: reclaimed when its scope ends",
// and "**The rule is uniform across body kinds**".  §23.3 (v0.359): "The
// implicit setter parameter is `value`.  It is a parameter of the setter" — a
// setter's block body is a BODY, with a parameter, and §28.3's rules govern
// bodies, not body KINDS.  §28.9 (v0.359): "**Accessor faces are uniform
// across type kinds.**"
//
// THE FIX.  The method-body pass, reused verbatim (`MethodOwnership::run`),
// exactly as `runOwnedSetterBody` (#5252) reuses it for the `set(owned)`
// spelling — so a setter body and a method body cannot disagree about a
// verdict.  The implicit `value` parameter is NOT seeded Owned: a plain setter
// takes a BORROW of the property's declared type (§23.3), so §23.3's rule that
// the setter MAY store `value` into a field survives — §28.3's
// borrowed-field-store rule is holder-kind-conditioned and a
// PARAMETER-sourced borrow is admissible there.
//
// ORACLE.  `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit.  Every row asserts
// the program's own output and exit value as well as the leak count, so a
// silent zero-fill cannot pass, and the leak row LOOPS (200 setter calls) so a
// one-shot leak cannot hide under the sanitizer's noise.
//
// THE EARLY-RETURN PATH IS NOT THIS ISSUE'S.  A plain setter body's `return;`
// is the inlined-accessor early exit, where the disposals ARE registered and
// skipped on the early exit — issue #9029 (parking-lot), which this issue
// names as a separate defect.  The rows below use the NORMAL exit, which is
// where #9073's leak lives.

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

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired.  `runAotChecked` reports an abort and any sanitizer report as
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

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    didNotCrash(r, exitCode, out);
    assertNoLeak(r);
}

// How many times the property is written.  A leak repro loops (lane contract:
// 50-500x) so a one-shot leak cannot hide under the sanitizer's noise.
constexpr int kWrites = 200;

// The issue's own repro, VERBATIM, with the write LOOPED (the issue's repro
// writes the property 200 times already; the leak is 16 B per write).
std::string issueRepro() {
    return std::string(
        "module M; "
        "import System; "
        "public class Res "
        "  public n: int := 0; "
        "  destructor "
        "    Console.Write(\"D\"); "
        "  end destructor; "
        "end Res; "
        "public class H "
        "  public constructor() end constructor; "
        "  public property P: int "
        "    set => do var r := new Res(); end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var h := new H(); "
        "    for var i in 0 ..< ") + std::to_string(kWrites) + " do "
        "      h.P := 1; "
        "    end for; "
        "    Console.Write(\"|\"); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
}

// The expected output of the repro with the fix: the setter body's scope-end
// disposal runs at every write, so the destructor prints "D" once per write,
// and the program prints "|" at the end.
std::string expectedDisposedThenBar(int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += "D";
    out += "|";
    return out;
}

// The EXPLICITLY-DISPOSED twin: `r` is disposed by hand inside the setter body
// (§28.3's consume table), so it is NOT undischarged at the scope end and the
// §28.3.10 scope-end disposal must not run a second time.  GREEN on the base
// as well (the explicit dispose already reclaims the block) — a BOUNDARY
// CONTROL, kept as a gtest row because a corpus case cannot express a
// dispose COUNT over 200 iterations; it goes RED under the plausible wrong
// fix "auto-drop every owned local in a setter body, ignoring the discharge
// set" (two "D" prints per write, and an ASan double-free report).
std::string explicitlyDisposedRepro() {
    return std::string(
        "module M; "
        "import System; "
        "public class Res "
        "  public n: int := 0; "
        "  destructor "
        "    Console.Write(\"D\"); "
        "  end destructor; "
        "end Res; "
        "public class H "
        "  public constructor() end constructor; "
        "  public property P: int "
        "    set => do var r := new Res(); r?.Dispose(); end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var h := new H(); "
        "    for var i in 0 ..< ") + std::to_string(kWrites) + " do "
        "      h.P := 1; "
        "    end for; "
        "    Console.Write(\"|\"); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
}

// The ADMITTING twin: §23.3 (v0.359) — "The setter's implicit `value`
// parameter has no modifier by default and takes a borrow of the property's
// declared type … the setter may store it."  A plain setter body that stores
// its `value` parameter into the backing field must stay ADMITTED: §28.3's
// borrowed-field-store rule is holder-kind-conditioned, and a
// PARAMETER-sourced borrow is admissible there (only a scope-bound local or a
// temporary is rejected).  GREEN on the base and GREEN with the fix — a
// BOUNDARY CONTROL against the plausible wrong fix "reject every field store
// in a setter body" (which would reject every ordinary setter).
std::string valueStoredIntoFieldRepro() {
    return std::string(
        "module M; "
        "import System; "
        "public class H "
        "  private _x: int := 0; "
        "  public constructor() end constructor; "
        "  public property P: int "
        "    get => _x; "
        "    set => _x := value; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var h := new H(); "
        "    h.P := 7; "
        "    Console.WriteLine(h.P); "
        "    return 0; "
        "  end Main; "
        "end Program; ");
}

// The FIELD-STORE row: a plain setter body that stores its scope-bound owned
// local into a field.  §28.3 rejects the byte-identical METHOD-body twin
// ("borrow of `reg` stored into a field would outlive its source — `reg` is an
// owned scope-bound local disposed at scope end, and a field of the target
// object outlives it"), so the setter body must reject it too.
std::string storesOwnedLocalIntoFieldRepro() {
    return std::string(
        "module M; "
        "import System; "
        "public class Reg2 "
        "  private _o: Object? := null; "
        "  public constructor() end constructor; "
        "  destructor "
        "    Console.Write(\"D\"); "
        "  end destructor; "
        "end Reg2; "
        "public class Fl "
        "  private _sink: Object? := null; "
        "  public constructor() end constructor; "
        "  public property P: int "
        "    set(v) => do var reg := new Reg2(); _sink := reg; end; "
        "  end; "
        "end Fl; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var fl := new Fl(); "
        "    fl.P := 1; "
        "    Console.Write(\"|\"); "
        "    return 0; "
        "  end Main; "
        "end Program; ");
}

}  // namespace

// THE REGRESSION ROW: the issue's own repro, verbatim, looped.  RED on the
// base (the destructor NEVER runs and 16 B leak per write), GREEN with the
// fix.
BEZEL_TEST(plain_setter_block_body_ownership_pass_9073,
           plain_setter_body_owned_local_is_reclaimed_at_scope_end) {
    auto r = runAotChecked(issueRepro());
    expectClean(r, 0, expectedDisposedThenBar(kWrites));
}

// THE REJECTION ROW: §28.3's borrowed-field-store rule in a plain setter
// body.  RED on the base (the program COMPILED), GREEN with the fix.
BEZEL_TEST(plain_setter_block_body_ownership_pass_9073,
           plain_setter_body_storing_its_owned_local_into_a_field_is_rejected) {
    auto r = runAot(storesOwnedLocalIntoFieldRepro());
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "stored into a field would outlive its source"));
}

// The EXPLICITLY-DISPOSED twin (a boundary control — green on both arms):
// exactly ONE "D" per write, so the scope-end disposal does not run on top of
// the explicit one.
BEZEL_TEST(plain_setter_block_body_ownership_pass_9073,
           plain_setter_body_explicitly_disposed_local_is_not_disposed_twice) {
    auto r = runAotChecked(explicitlyDisposedRepro());
    expectClean(r, 0, expectedDisposedThenBar(kWrites));
}

// The ADMITTING twin (a boundary control — green on both arms): a plain
// setter body that stores its `value` parameter into the backing field stays
// ADMITTED and runs (§23.3: "the setter may store it").
BEZEL_TEST(plain_setter_block_body_ownership_pass_9073,
           plain_setter_body_may_store_its_value_parameter_into_the_backing_field) {
    auto r = runAot(valueStoredIntoFieldRepro());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());
}
