// Regression tests for #6721's store leg: §9.4 (v0.340) "the store rejection
// above reads a lambda body inside the member body as the member body, and one
// inside a value kind's constructor body as a member body too, a function
// value outliving the construction window (§22.8) — a chain rooted at the
// captured `this` that reaches the value is rejected there."
//
// A record constructor body is not a receiver position (§9.4, *Constructor
// bodies are not receiver positions*): it assigns its members directly. A
// lambda formed there captures a COPY of `this` (§9.4), so a store through it
// would land in the copy, not in the value under construction. Before the fix
// the constructor-lambda store was accepted (the member-body form already was
// rejected): `new P(1).X` printed 1 with the store silently lost.
//
// per §9.4, §22.8 (v0.340)

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}
constexpr const char* kCtorLambdaStore =
    "to the receiver's own storage in a lambda body inside a record "
    "constructor";
}  // namespace

// The reviewer's probe (p5/s02): a bare member store in a constructor lambda.
BEZEL_TEST(record_ctor_lambda_receiver_store_6721,
           bare_member_store_in_ctor_lambda_rejected_spec_9_4) {
    auto r = runAot(
        "module M; "
        "public record P "
        "  public property X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "    var f := lambda() => do X := 5; end; "
        "    f(); "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(new P(1).X); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kCtorLambdaStore));
}

// The explicit `this.X` spelling and a compound assignment.
BEZEL_TEST(record_ctor_lambda_receiver_store_6721,
           explicit_this_compound_store_in_ctor_lambda_rejected_spec_9_4) {
    auto r = runAot(
        "module M; "
        "public record P "
        "  public property X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "    var f := lambda() => do this.X += 5; end; "
        "    f(); "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(new P(1).X); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kCtorLambdaStore));
}

// Control (passes before the fix too): the explicit `this.X` store in a lambda
// inside a record METHOD body, the form §9.4 already rejected.
BEZEL_TEST(record_ctor_lambda_receiver_store_6721,
           explicit_this_store_in_method_lambda_rejected_spec_9_4) {
    auto r = runAot(
        "module M; "
        "public record P "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public method Bump(): int "
        "    var f := lambda() => do this.X := 1; end; "
        "    f(); "
        "    return X; "
        "  end Bump; "
        "end P; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(new P(3).Bump()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "to the receiver's own storage in a record method body"));
}

// Over-rejection guards: the constructor body's own store (§9.4 *Constructor
// bodies are not receiver positions*), a lambda in the constructor that stores
// to its own local, and a lambda that stores to a static member.
BEZEL_TEST(record_ctor_lambda_receiver_store_6721,
           ctor_body_store_and_lambda_local_store_accepted_spec_9_4) {
    auto r = runAot(
        "module M; "
        "public record P "
        "  public property X: int; "
        "  public static Hits: int; "
        "  public constructor(x: int) "
        "    var k := 0; "
        "    var f := lambda() => do k := k + 2; Hits := Hits + 1; end; "
        "    f(); "
        "    X := x + k; "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(new P(1).X); "
        "    Console.WriteLine(P.Hits); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n1\n"));
}
