// Regression tests for #8864: an INLINED property setter called in a loop keeps
// handing a capturing lambda the FIRST iteration's `value` once an earlier
// call in the setter body splits the basic block.
//
// Spec (v0.359):
//  - §23.3: "The implicit setter parameter is `value`. It is a parameter of
//    the setter" — every invocation binds it to that invocation's assigned
//    value (§23.4: an indexer's parameters likewise, per access).
//  - §27.5: "Captured variables are heap-allocated in a closure object", and
//    "each execution of its declaration binds a fresh variable, so a lambda
//    formed after it captures that execution's variable".
//
// An accessor body is inlined into the caller, so its parameter binding
// re-executes in the caller's function on every pass of a loop. boxLocalSlot
// took fresh-per-pass boxing only when the binding store sat in the capture's
// own basic block; a call earlier in the body (`var q := g();`) splits it, the
// guarded single-cell strategy then allocated the capture cell once, and every
// later pass read the first pass's value. MEASURED on r30-c1 (e1bbf3711): the
// issue's repro printed `10 11 20 20` (expected `10 11 20 21`), a static setter
// called 200x summed to 0 (expected 19900).
//
// Each row loops the accessor call at least 50x and asserts the exact stdout.
//
// per §23.3, §23.4, §27.5 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string stdoutOf(const std::string& program) {
    auto r = runAot(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
}  // namespace

// The issue's literal repro (instance setter), its loop extended to 50 passes.
BEZEL_TEST(inlined_accessor_param_capture_fresh_8864,
           instance_setter_value_capture_per_call_50x_spec_23_3_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property P: int "
        "    get => n; "
        "    set => do var g := lambda(): int => 3; var q := g(); "
        "      var k := lambda(): int => value; n := k(); end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    h.P := 10; Console.Write(h.n); Console.Write(\" \"); "
        "    h.P := 11; Console.Write(h.n); Console.Write(\" \"); "
        "    var i := 0; var t := 0; "
        "    while i < 50 do h.P := 20 + i; t := t + h.n; i := i + 1; end; "
        "    Console.Write(t); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "10 11 2225");
}

// Review probe s12: the STATIC-property setter path, 200 passes.
BEZEL_TEST(inlined_accessor_param_capture_fresh_8864,
           static_setter_value_capture_per_call_200x_spec_23_3_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public static n: int; "
        "  public static property P: int "
        "    get => n; "
        "    set => do var g := lambda(): int => 3; var q := g(); "
        "      var h := lambda(): int => value; n := h(); end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; var tot := 0; "
        "    while i < 200 do H.P := i; tot := tot + H.P; i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "19900");
}

// Review probe s13: the capture sits in a loop INSIDE the setter body.
BEZEL_TEST(inlined_accessor_param_capture_fresh_8864,
           setter_inner_loop_value_capture_200x_spec_23_3_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property P: int "
        "    get => n; "
        "    set => do var j := 0; while j < 3 do "
        "      var g := lambda(): int => value + j; n := g(); j := j + 1; "
        "    end; end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; var tot := 0; "
        "    using var h := new H(); "
        "    while i < 200 do h.P := i; tot := tot + h.P; i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "20300");
}

// The inlined INDEXER setter (lowerIndexerSet): `value` and the index `i`.
BEZEL_TEST(inlined_accessor_param_capture_fresh_8864,
           indexer_setter_param_capture_per_call_50x_spec_23_4_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => n + i; "
        "    set => do var g := lambda(): int => 3; var q := g(); "
        "      var k := lambda(): int => value + i; n := k(); end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var i := 0; var t := 0; "
        "    while i < 50 do h[i] := 10 * i; t := t + h.n; i := i + 1; end; "
        "    Console.Write(t); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "13475");
}

// The compound-assignment INDEXER path (lowerIndexerCompoundCore).
BEZEL_TEST(inlined_accessor_param_capture_fresh_8864,
           indexer_compound_param_capture_per_call_50x_spec_23_4_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => n; "
        "    set => do var g := lambda(): int => 3; var q := g(); "
        "      var k := lambda(): int => value + i; n := k(); end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var i := 0; "
        "    while i < 50 do h[i] += 1; i := i + 1; end; "
        "    Console.Write(h.n); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "1275");
}

// The inlined expression-bodied INDEXER getter (lowerIndexerGet).
BEZEL_TEST(inlined_accessor_param_capture_fresh_8864,
           indexer_getter_param_capture_per_call_50x_spec_23_4_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int => (var g := lambda(): int => 3; "
        "    var q := g(); var k := lambda(): int => i + q; k()); "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var i := 0; var t := 0; "
        "    while i < 50 do t := t + h[i]; i := i + 1; end; "
        "    Console.Write(t); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "1375");
}
