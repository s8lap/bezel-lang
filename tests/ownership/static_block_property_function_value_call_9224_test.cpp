// Regression test for issue #9224 — a call through a STATIC, BLOCK-FORM
// function-typed property (`Hx.U(1)`, where `U` is declared
// `public static property U: (int) -> int get => … end;`) fails to lower and
// yields 0 (or crashes), behind an `internal:` warning.
//
// THE DEFECT. A property's READ is one of two shapes (§23.2 / §23.3, v0.359):
// the EXPRESSION-bodied form, whose body is lowered in the READER's frame, and
// the BLOCK form, whose body is emitted OUT OF LINE as a standalone function
// so that a `return` inside it terminates only the getter (§21.7). The block
// form's read site lowers the RECEIVER first and passes it to the getter as
// its `this` argument.
//
// A STATIC property has no receiver: §22.12 (v0.359) declares the member
// `static`, and the static-context restriction it rests on (spec §2.7's
// *Enclosing-type members*, v0.359) says "a static body has no `this`". So
// for `Hx.U` the receiver is the TYPE NAME `Hx` — not a value — `lowerExpr`
// yields null, and the whole read fails to lower.
//
// MEASURED on the base (r30-c1 @ fc77e935f), each of these is broken there:
//   * `t := t + Hx.U(1);`      → `internal: assignment RHS expression failed
//     to lower to a value … — #2961`, the store is DROPPED, and the program
//     prints `0` where it must print the sum.
//   * `var g := Hx.U; g(1);`   → `internal: lowerLocalDecl initializer
//     expression failed to lower … — #2434`, `g` is ZERO-INITIALIZED, and the
//     call then SEGFAULTS on the null closure.
//   * the BARE spelling `U` inside the declaring class → the compiler emits
//     `call ptr @"bz$prop$U$get"(<null operand!>)` and ABORTS with
//     "IR verification failed" — a compile-time crash, not a wrong value.
//   * `Apply(Hx.U)` (the property as a call argument) → `internal: lowerExpr
//     returned null for call argument in buildOrderedArgs — #1386`.
//   * a plain read of a static block-form `int` property → `internal: the
//     argument of this `Console.WriteLine` failed to lower to a value … —
//     #3079`, and NOTHING is printed.
//
// The INSTANCE spelling of the same shape (`o.P(1)`) works — its receiver is
// a value — and so does the static EXPRESSION-bodied spelling (`Hx.T(1)`):
// the expression-getter read site has had a static branch since #5319, and
// the block-form read site never got one.
//
// THE SPEC.
//   §22.12 (v0.359), *Static members*: "Static methods, properties, fields,
//     and constructors are declared with `static`."
//   §2.7 (v0.359), *Enclosing-type members*: the static-context restriction
//     — "a static body has no `this`".
//   §23.3 (v0.359), *Block-form properties*: each accessor is `get =>
//     expression` or `get => do … end;`; a read invokes the getter.
//   §27.6 / §27.7 (v0.359): reading the property yields its function value,
//     and invoking that value calls that function.
//   §28.9 (v0.359), *Runtime-managed types*: runtime management governs the
//     function value itself — the heap-allocated closure object that backs
//     it — so a read that hands the reader a reference must have that
//     reference discharged.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that neither sanitizer fired, so a silent
// zero-fill cannot pass. The rows LOOP (200 iterations inside the program),
// so neither the defect nor a leak can hide under the sanitizer's noise.
//
// `runAot` CANNOT be the oracle here: it does not run the AddressSanitizer
// pass over the generated module (only `runAotChecked` sets
// `opts.sanitizeAddress`), so a leak would go unreported and the row would
// be green on the base — pinning nothing.

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

// ROW 1 — the issue's literal repro (`/var/tmp/bezel-scratch/r43-rev-m94-9175/
// probes/p13_static.bzl`): a static EXPRESSION-bodied property `T` and a
// static BLOCK-bodied property `U`, both called directly, summed over 200
// iterations. `T(1)` is 3 and `U(1)` is 4, so the sum is 200 × 7 = 1400.
//
// RED on the base: the `U` read fails to lower, so #2961 drops the WHOLE
// store and the program prints `0` (a silent zero-fill — the miscompile the
// issue reports).
BEZEL_TEST(static_block_property_function_value_call_9224,
           static_block_property_call_is_the_issue_repro) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public static property T: (int) -> int => lambda(x: int) => x + 2; "
        "  public static property U: (int) -> int "
        "    get => do return lambda(x: int) => x + 3; end; "
        "  end U; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      t := t + Hx.T(1) + Hx.U(1); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// ROW 2 — the static get-only (block-form) property READ on its own: the
// value is bound to a local and the local is called. `U(1)` is 4, so the sum
// over 200 iterations is 800.
//
// RED on the base: the read fails to lower, so #2434 zero-initializes `g` and
// the call SEGFAULTS on the null closure — the row fails on the abort, and
// the sanitizer leg is what would report it.
BEZEL_TEST(static_block_property_function_value_call_9224,
           static_block_property_read_into_a_local_then_called) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public static property U: (int) -> int "
        "    get => do return lambda(x: int) => x + 3; end; "
        "  end U; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var g := Hx.U; t := t + g(1); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "800\n");
}

// ROW 3 — the static property BACKED BY A STATIC FIELD set at runtime: the
// field's initializer runs lazily (§22.13's lazy static initialization), and
// the block-form getter hands the reader the field's function value.
//
// `F(1)` is 6, so the sum over 200 iterations is 1200.
//
// RED on the base: the block-form read fails to lower (#2961 drops the store,
// so the program prints `0`).
BEZEL_TEST(static_block_property_function_value_call_9224,
           static_block_property_backed_by_a_static_field) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  private static readonly _f: (int) -> int := lambda(x: int) => x + 5; "
        "  public static property F: (int) -> int "
        "    get => do return _f; end; "
        "  end F; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      t := t + Hx.F(1); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1200\n");
}

// ROW 4 — a CAPTURING lambda stored in the static property: the getter
// captures a local of its own (`k`), so each read mints a closure whose
// environment holds a heap-allocated cell (§27.5). The capture is what makes
// the row a leak/UAF row: 200 reads mint 200 closure cells, and every one of
// them must be discharged (§28.9).
//
// `F(1)` is 1 + 7 = 8, so the sum over 200 iterations is 1600.
//
// RED on the base: the read fails to lower (#2961 drops the store).
BEZEL_TEST(static_block_property_function_value_call_9224,
           static_block_property_holding_a_capturing_lambda) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public static property F: (int) -> int "
        "    get => do "
        "      var k := 7; "
        "      return lambda(x: int) => x + k; "
        "    end; "
        "  end F; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      t := t + Hx.F(1); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1600\n");
}

// ROW 5 — the BARE spelling of the static property inside its declaring type
// (`U`, not `Hx.U`): §2.7's *Enclosing-type members* admits a static member
// by bare name, so this is the same read by another route.
//
// `U(1)` is 4, so the sum over 200 iterations is 800.
//
// RED on the base: the compiler emits `call ptr @"bz$prop$U$get"(<null
// operand!>)` and ABORTS with "IR verification failed" — the row fails on the
// compile.
BEZEL_TEST(static_block_property_function_value_call_9224,
           static_block_property_read_bare_inside_its_declaring_type) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public static property U: (int) -> int "
        "    get => do return lambda(x: int) => x + 3; end; "
        "  end U; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      t := t + U(1); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Hx;";
    expectClean(runAotChecked(src), 0, "800\n");
}

// ROW 6 — the static property as a CALL ARGUMENT (`Apply(Hx.U)`): the read is
// in argument position, where §27.6's target-typed promotion and the
// call-argument protocol both apply.
//
// `Apply` calls its function-typed parameter, so the value is 4; over 200
// iterations the sum is 800.
//
// RED on the base: the read fails to lower, so #1386 reports "lowerExpr
// returned null for call argument" and the program never runs.
BEZEL_TEST(static_block_property_function_value_call_9224,
           static_block_property_passed_as_a_call_argument) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public static property U: (int) -> int "
        "    get => do return lambda(x: int) => x + 3; end; "
        "  end U; "
        "  public static method Apply(f: (int) -> int): int => f(1); "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      t := t + Hx.Apply(Hx.U); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "800\n");
}

// CONTROL 1 (base-green, named in the commit body) — the INSTANCE spelling of
// the very same block-form property, called directly: `o.P(1)`. The issue
// names it as the control that already works, and it must KEEP working.
//
// GREEN on the base and with the fix. It is red under the plausible wrong fix
// this commit's shape invites — "make EVERY block-form getter receiver-less"
// — which would break the instance read (the getter would be called with no
// `this` while its body reads the receiver's fields). It is also red under
// "lower the receiver for a static property too" (the base's own bug), which
// would leave the instance arm intact — so it pins the instance arm against
// the widening this fix must not make.
BEZEL_TEST(static_block_property_function_value_call_9224,
           instance_block_property_call_is_unaffected) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public property P: (int) -> int "
        "    get => do return lambda(x: int) => x + 3; end; "
        "  end P; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    using var o := new Hx(); "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      t := t + o.P(1); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "800\n");
}

// CONTROL 2 (base-green, named in the commit body) — the static
// EXPRESSION-bodied spelling, called directly: `Hx.T(1)`. This is the exact
// spelling the issue's title names, and it is the arm the fix must NOT
// disturb (the expression-getter read site has had its static branch since
// #5319).
//
// `T(1)` is 3, so the sum over 200 iterations is 600.
//
// GREEN on the base and with the fix. It is red under the plausible wrong fix
// "route every static property read through the new block-form arm" — which
// would lower the receiver of an expression-bodied static property (a TYPE
// name) and fail the read exactly as the base fails the block form.
BEZEL_TEST(static_block_property_function_value_call_9224,
           static_expression_property_call_is_unaffected) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public static property T: (int) -> int => lambda(x: int) => x + 2; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      t := t + Hx.T(1); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "600\n");
}
