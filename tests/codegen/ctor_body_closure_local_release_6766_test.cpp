// Regression tests for #6766: a lambda-valued local in a constructor body
// (instance or static) leaks its closure.
//
// Spec (v0.359):
//  - §27.5: "Captured variables are heap-allocated in a closure object";
//  - §28.9 (Function-typed values): "Runtime management governs the
//    *function value itself* — the heap-allocated closure object that backs
//    it", whose "lifetime is managed by the runtime without user
//    involvement", and "a plain `var` of function type scopes its captures
//    identically" (to a `using var`, i.e. to its enclosing scope).
// A closure local formed in a constructor body and dropped at the body's end
// holds the only reference; its environment must be released at scope end,
// exactly as it is in an ordinary method body (the same lambda in a record
// METHOD is clean: value_receiver_this_capture_by_copy_6721
// `record_local_closure_invoked_in_body_200x_spec_9_4`).
//
// Before the fix the constructor body lowering never ran the closure-local
// scope-end analysis (`collectClosureReleaseEligible` and its token), so the
// local's closure was never released: MEASURED 80 B in 3 allocations for one
// `new P(7)`, 8000 B / 300 allocations over 100 constructions.
//
// Rows run under runAotChecked (checked disposal + generated-code ASan +
// leak check); constructions loop so the leak cannot hide in sanitizer noise.
//
// per §27.5, §28.9, §22.7, §22.12 (v0.359)

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

// The issue's literal repro: a record constructor, lambda captures `this`.
BEZEL_TEST(ctor_body_closure_local_release_6766,
           record_ctor_this_capturing_closure_local_released_100x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record P "
        "  public readonly property X: int; "
        "  public readonly property Y: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "    var f := lambda(): int => this.X; "
        "    Y := f(); "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main() "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 100 do "
        "      sum := sum + new P(i).Y; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(sum); "
        "  end Main; "
        "end Program;"),
        "4950\n");
}

// Issue scope note: the leak does not depend on `this` — a record constructor
// whose lambda captures only a local.
BEZEL_TEST(ctor_body_closure_local_release_6766,
           record_ctor_local_capturing_closure_local_released_100x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record P "
        "  public readonly property Y: int; "
        "  public constructor(x: int) "
        "    var k := x; "
        "    var f := lambda(): int => k + 1; "
        "    Y := f(); "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main() "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 100 do "
        "      sum := sum + new P(i).Y; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(sum); "
        "  end Main; "
        "end Program;"),
        "5050\n");
}

// Issue scope note: a CLASS constructor leaks the same way.
BEZEL_TEST(ctor_body_closure_local_release_6766,
           class_ctor_closure_local_released_100x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class P "
        "  public Y: int; "
        "  public constructor(x: int) "
        "    var f := lambda(): int => x * 2; "
        "    Y := f(); "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main() "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 100 do "
        "      using var p := new P(i); "
        "      sum := sum + p.Y; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(sum); "
        "  end Main; "
        "end Program;"),
        "9900\n");
}

// Static-constructor sibling (the issue's second comment). A static
// constructor runs once (§22.12), so the leak is one-shot-shaped and a loop
// cannot be used; the ASan leg's leak check on the exit is the oracle.
BEZEL_TEST(ctor_body_closure_local_release_6766,
           static_ctor_closure_local_released_spec_22_12_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "class C "
        "  static constructor() "
        "    var sb := new StringBuilder(); "
        "    _ := sb.Append(\"sbtext\"); "
        "    var o: Object := sb; "
        "    var f := lambda() => do o := \"instatic\"; end; "
        "    f(); "
        "    Console.WriteLine(o); "
        "  end constructor; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "instatic\n");
}

// Accessor-body sibling (the issue's third comment): a block getter's
// lambda-valued local.
BEZEL_TEST(ctor_body_closure_local_release_6766,
           getter_body_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class B "
        "  public property P: int "
        "    get => do "
        "      var f := lambda(): int => 8485; "
        "      return f(); "
        "    end; "
        "  end P; "
        "end B; "
        "public class Program "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 50 do "
        "      using var b := new B(); "
        "      sum := sum + b.P; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(sum); "
        "    return 0; "
        "  end Main; "
        "end Program;"),
        "424250\n");
}
