// Regression tests for #8762: a lambda-valued local in a PROPERTY SETTER
// body (and in the indexer accessor bodies) leaks its closure.
//
// Spec (v0.359):
//  - §27.5: "Captured variables are heap-allocated in a closure object";
//  - §28.9 (Function-typed values): "Runtime management governs the
//    *function value itself* — the heap-allocated closure object that backs
//    it", whose "lifetime is managed by the runtime without user
//    involvement", and "a plain `var` of function type scopes its captures
//    identically" (to a `using var`, i.e. to its enclosing scope).
// A closure local formed in an accessor body and dropped at that body's end
// holds the only reference; its environment must be released at scope end,
// exactly as it is in an ordinary method body.
//
// The accessor bodies did not run the closure-local scope-end analysis
// (`collectClosureReleaseEligible` and its per-function token), so the local's
// closure was never released: MEASURED on r30-c1 (1128406b7) with
// `bezelc --sanitize=address` on the issue's literal repro —
// "SUMMARY: AddressSanitizer: 4196 byte(s) leaked in 149 allocation(s)" over
// 50 setter calls (the closure cell, its environment, and the capture cell
// for the setter's implicit `value` parameter).
//
// Rows run under runAotChecked (checked disposal + generated-code ASan +
// leak check); the accessor calls loop so the leak cannot hide in sanitizer
// noise.
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

// The issue's literal repro: a class property whose BLOCK-form setter forms
// a lambda-valued local and calls it.
BEZEL_TEST(accessor_body_closure_local_release_8762,
           property_setter_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property P: int "
        "    get => n; "
        "    set => do "
        "      var g := lambda(): int => value + 1; "
        "      n := g(); "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    using var h := new H(); "
        "    while i < 50 do h.P := i; i := i + 1; end; "
        "    Console.Write(h.P); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "50");
}

// The indexer SETTER twin (the issue names the indexer accessors too).
BEZEL_TEST(accessor_body_closure_local_release_8762,
           indexer_setter_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => n; "
        "    set => do "
        "      var g := lambda(): int => value + 1; "
        "      n := g(); "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    using var h := new H(); "
        "    while i < 50 do h[0] := i; i := i + 1; end; "
        "    Console.Write(h[0]); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "50");
}

// The indexer GETTER twin — the indexer spelling of the block getter #6766
// fixed for a PROPERTY (`ensureIndexerBlockGetter`).
BEZEL_TEST(accessor_body_closure_local_release_8762,
           indexer_getter_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => do "
        "      var g := lambda(): int => 7; "
        "      return n + g(); "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var t := 0; "
        "    using var h := new H(); "
        "    while i < 50 do t := t + h[0]; i := i + 1; end; "
        "    Console.Write(t); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "350");
}

// The STATIC-property setter twin (§22.12 / §22.13) — the other inlined
// setter path in `lowerPropertySetValue`.
BEZEL_TEST(accessor_body_closure_local_release_8762,
           static_property_setter_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public static n: int; "
        "  public static property P: int "
        "    get => n; "
        "    set => do "
        "      var g := lambda(): int => value + 1; "
        "      n := g(); "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    while i < 50 do H.P := i; i := i + 1; end; "
        "    Console.Write(H.P); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "50");
}
