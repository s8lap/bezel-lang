// Regression tests for #8763: a lambda-valued local in a FUNCTION-TYPED block
// getter leaks its closure, once per read.
//
// Spec (v0.359):
//  - §27.5: "Captured variables are heap-allocated in a closure object";
//  - §28.9: "Runtime management governs the *function value itself* — the
//    heap-allocated closure object that backs it", whose "lifetime is managed
//    by the runtime without user involvement", and "a plain `var` of function
//    type scopes its captures identically" (to its enclosing scope);
//  - §28.9: "A getter body whose result is a fresh runtime-managed production
//    … remains admissible — runtime-managed values are exempt from the
//    ceremony — and the implementation reclaims the production; **how** is
//    not specified".
// The closure local formed in the getter body and handed to the reader holds
// the only reference to its closure object; the implementation reclaims it,
// and the reference the reader holds is discharged at the reader's scope end.
//
// The getter body did not run the per-body closure-local analysis and did not
// publish its declared return type, so the getter neither released its own
// closure locals nor minted the reader's reference: MEASURED on r30-c1
// (1128406b7) with `bezelc --sanitize=address` on the issue's literal repro —
// "SUMMARY: AddressSanitizer: 3600 byte(s) leaked in 100 allocation(s)" over
// 50 reads.
//
// Rows run under runAotChecked (checked disposal + generated-code ASan +
// leak check); the getter is read in a loop so the leak cannot hide in
// sanitizer noise.
//
// per §27.5, §28.9, §23.3, §23.4 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string stdoutChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
}  // namespace

// The issue's literal repro: a function-typed property whose BLOCK-form getter
// forms a lambda local (capturing `this`) and returns it.
BEZEL_TEST(property_block_getter_closure_release_8763,
           block_getter_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public constructor(x: int) n := x; end; "
        "  public property Mk: () -> int "
        "    get => do "
        "      var g := lambda(): int => n + 1; "
        "      return g; "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(3); "
        "    while i < 50 do var f := h.Mk; tot := tot + f(); i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "200");
}

// The capture-free twin of the same shape — the closure's environment is
// empty, so the leak is the closure cell alone.
BEZEL_TEST(property_block_getter_closure_release_8763,
           block_getter_capture_free_closure_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public property Mk: () -> int "
        "    get => do "
        "      var g := lambda(): int => 42; "
        "      return g; "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(); "
        "    while i < 50 do var f := h.Mk; tot := tot + f(); i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "2100");
}

// The getter body also holds a closure local that is NOT returned: the
// scope-end release of THAT local is the #6766 mechanism extended to a
// function-typed getter (the body is analysed like any other body).
BEZEL_TEST(property_block_getter_closure_release_8763,
           block_getter_unreturned_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public constructor(x: int) n := x; end; "
        "  public property Mk: () -> int "
        "    get => do "
        "      var spare := lambda(): int => 7; "
        "      var g := lambda(): int => n + 1; "
        "      return g; "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(3); "
        "    while i < 50 do var f := h.Mk; tot := tot + f(); i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "200");
}
