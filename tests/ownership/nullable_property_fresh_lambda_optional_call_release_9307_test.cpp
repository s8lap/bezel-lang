// Regression test for issue #9307 — `P?(this)` over a NULLABLE function-typed
// property whose EXPRESSION-bodied getter returns a FRESH lambda leaked the
// lambda (72 B per read: the 64 B closure cell + its 8 B environment); the
// non-nullable spelling `P(this)` was clean.
//
// THE RULE. §28.9 (v0.359): every function value holder releases exactly one
// reference — "runtime management governs the function value itself, the
// heap-allocated closure object that backs it". §23.2 inlines an
// expression-bodied getter in the READER's frame, so the lambda literal's cell
// is allocated there and the reader owns that +1; `P?(...)` is a call through
// that value (§7.5: the optional call is skipped when the value is null), and
// the call's end is where §28.3.11 discharges the reader's reference.
//
// THE DEFECT, in two places. (1) `propertyReadMintsClosureRef`
// (src/codegen/LlvmBackend_Setup.cpp) refused a NULLABLE declared function type
// for the expression-bodied getter, with the BLOCK getter's reason ("no owner
// for a closure reference", #8763's fix-forward 2) — the expression form has
// no such variant. (2) The `NullCondCallExpr` lowering
// (src/codegen/LlvmBackend_Exprs.cpp) had no mint-release arm, only the
// holder-lend retain/release of #9123; the non-nullable twin has one
// (`calleeMintsRef`, LlvmBackend_Calls2.cpp).
//
// THE FIX. The predicate admits the nullable declared type for the
// expression-bodied form only (the block form returns earlier and keeps its
// own gate), and the optional call releases the reader's reference on the CALL
// edge after the call (the null edge minted nothing). Lifting the predicate
// also makes the other discharge sites (binding local, returned read) own the
// nullable read's +1 — rows 3-4 pin those spellings. The CROSS-MODULE read of a function-typed property
// leaks for the non-nullable type too (#9296, imported getter path) and is not
// this fix's; no row pins it here.
//
// ORACLE. `runAotChecked` (ASan over the generated
// module + LeakSanitizer); each loop runs 200x so a leak cannot hide under
// sanitizer noise, and each row asserts the program's own output.
//
// per §28.9, §28.3.11, §23.2, §7.5 (issue #9307)

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

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

}  // namespace

// ROW 1 — the ISSUE'S OWN REPRO (`s_fresh_nullc.bzl`): the optional call over a
// nullable expression-bodied property returning a fresh lambda, 200 reads.
// Each read returns 7 (`Fill` is a no-op), so 1400.
//
// RED on the base: 12800 B direct (+ indirect) leaked in 200 allocations.
BEZEL_TEST(nullable_property_fresh_lambda_optional_call_release_9307,
           issue_repro_optional_call_releases_the_fresh_lambda) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public property P: ((Hx) -> int)? => lambda(x: Hx) => 7; "
        "  public constructor() end constructor; "
        "  public method Fill(k: int) end Fill; "
        "  public method Run(): int "
        "    return P?(this) ?? 0; "
        "  end Run; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); h.Fill(i mod 3); t := t + h.Run(); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// ROW 2 — the optional call with an ARGUMENT, through the QUALIFIED spelling
// `h.Q?(2)` and a second receiver: the release is on the call edge whatever
// the callee spelling or argument list. Q adds 5 and P adds n+1 = 4 (n = 3);
// 200 x (7 + 4) = 2200.
//
// RED on the base: one cell leaked per optional call.
BEZEL_TEST(nullable_property_fresh_lambda_optional_call_release_9307,
           optional_call_with_arguments_and_qualified_callee_releases) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public n: int; "
        "  public property P: ((Hx) -> int)? => lambda(x: Hx) => x.n + 1; "
        "  public property Q: ((int) -> int)? => lambda(k: int) => k + 5; "
        "  public constructor() n := 3; end constructor; "
        "  public method ViaQ(): int return Q?(2) ?? 0; end ViaQ; "
        "  public method ViaMember(h: Hx): int return h.P?(h) ?? 0; end ViaMember; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); "
        "      t := t + h.ViaQ() + h.ViaMember(h); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "2200\n");
}

// ROW 3 — the read BOUND to a local (`var f := P;`) then called through the
// local. The predicate now admits the nullable read, so the binding local owns
// the +1 and releases it at scope end. 200 x 4 = 800.
//
// RED on the base: the nullable read was not a producer, so the cell leaked.
BEZEL_TEST(nullable_property_fresh_lambda_optional_call_release_9307,
           nullable_read_bound_to_a_local_releases_at_scope_end) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public n: int; "
        "  public property P: ((Hx) -> int)? => lambda(x: Hx) => x.n + 1; "
        "  public constructor() n := 3; end constructor; "
        "  public method ViaLocal(): int "
        "    var f := P; "
        "    return f?(this) ?? 0; "
        "  end ViaLocal; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); t := t + h.ViaLocal(); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "800\n");
}

// ROW 4 — the read RETURNED (`return P;`) and called by the caller: the +1
// transfers to the caller's local (§28.3.11), released at its scope end — once.
// 200 x 4 = 800.
//
// RED on the base: the cell leaked.
BEZEL_TEST(nullable_property_fresh_lambda_optional_call_release_9307,
           nullable_read_returned_to_the_caller_is_owned_once) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public n: int; "
        "  public property P: ((Hx) -> int)? => lambda(x: Hx) => x.n + 1; "
        "  public constructor() n := 3; end constructor; "
        "  public method Get(): ((Hx) -> int)? "
        "    return P; "
        "  end Get; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); "
        "      var g := h.Get(); "
        "      t := t + (g?(h) ?? 0); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "800\n");
}

// CONTROL — the over-release guard the fix needs: a nullable property whose
// getter returns a STORED FIELD closure (a borrow: the field owns the only
// reference), an auto-property, and a NULL property, all called through `?(...)`.
// A wrong lift that released a borrowed read would free the field's closure
// under the next call (ASan: heap-use-after-free / double free). B adds n+2 =
// 5, A adds n+1 = 4, Z is null: -1. 200 x 8 = 1600.
BEZEL_TEST(nullable_property_fresh_lambda_optional_call_release_9307,
           borrowed_and_null_nullable_properties_are_not_over_released) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public n: int; "
        "  private _q: ((Hx) -> int)?; "
        "  public property A: ((Hx) -> int)? := lambda(x: Hx) => x.n + 1; "
        "  public property B: ((Hx) -> int)? => _q; "
        "  public property Z: ((Hx) -> int)? => null; "
        "  public constructor() n := 3; _q := lambda(x: Hx) => x.n + 2; end constructor; "
        "  public method RunA(): int return A?(this) ?? 0; end RunA; "
        "  public method RunB(): int return B?(this) ?? 0; end RunB; "
        "  public method RunZ(): int return Z?(this) ?? -1; end RunZ; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); "
        "      t := t + h.RunA() + h.RunB() + h.RunZ(); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1600\n");
}
