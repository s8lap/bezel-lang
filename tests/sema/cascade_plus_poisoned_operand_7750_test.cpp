// Issue #7750 (campaign `builtin-call-admission`): after an unresolved type
// name or an unknown member, the untyped operand of a `string + …` made §4.6's
// string-operand rule print a CASCADE `+` error (once with no location,
// `:0:0:`). The cascade is now suppressed — but ONLY when an error has already
// been reported, because an operand is also left untyped with NO error at all,
// and there the `+` error is the program's only rejection.
//
// THE GOVERNING TEXT. Language spec §4.6 (v0.355): `+` on strings is
// concatenation of two `string` operands; the compiler does not implicitly
// convert another type to `string`. So `"x=" + G` (a method group),
// `"x=" + System` (a namespace) and `"x=" + Console.WriteLine("a")` (a call
// with no value) are compile errors.
//
// THE ORACLE. A rejection row asserts the `+` diagnostic's TEXT at its REAL
// line:column, that it is reported EXACTLY ONCE, and that the compiler did not
// crash. A cascade row asserts the program is still REJECTED by its first
// error (so the diagnostics are filled and the absence assert is not vacuous)
// and that the `+` text is absent.
//
// The four rejection rows are the review's silent-acceptance probes
// (r39-rev-7750 p04, p12, p19, p20): the first #7750 commit's guard deleted the
// only rejection, and each program COMPILED (printing `x=`, nothing, `a`, and
// `a0`).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::diagnosticMessageCount;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

constexpr const char* kPlus =
    "`+` requires both operands to be `string` \xE2\x80\x94 the compiler does "
    "not implicitly convert other types to `string`; use interpolation "
    "(`$\"...{x}...\"`) or `.ToString()` (\xC2\xA7""4.6)";

// The `+` error, at `loc` (":line:col: "), exactly once, at compile time.
void expectPlusRejectedOnceAt(const JitResult& r, const std::string& loc) {
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, (loc + kPlus).c_str()));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kPlus), 1);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// Rejected by its FIRST error `first`, with no `+` cascade and no `:0:0:` line.
void expectNoPlusCascade(const JitResult& r, const char* first) {
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, first));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kPlus), 0);
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, ":0:0:"), 0);
}

}  // namespace

// --- an untyped operand with NO prior error is still refused (§4.6) ---

// p04: a static METHOD GROUP is not a `string`.
BEZEL_TEST(cascade_plus_poisoned_operand_7750,
           string_plus_static_method_group_is_refused) {
    auto r = runAot(R"(module M;
import System;
class C
  public static method G(): int => 5;
  public static method Main(): int
    Console.WriteLine("x=" + G);
    return 0;
  end Main;
end C;
)");
    expectPlusRejectedOnceAt(r, ":6:23: ");
}

// p12: a NAMESPACE used as a value is not a `string`.
BEZEL_TEST(cascade_plus_poisoned_operand_7750,
           string_plus_namespace_is_refused) {
    auto r = runAot(R"(module M;
import System;
class C
  public static method Main(): int
    Console.WriteLine("x=" + System);
    return 0;
  end Main;
end C;
)");
    expectPlusRejectedOnceAt(r, ":5:23: ");
}

// p19: a builtin call with NO value (`Console.WriteLine`) is not a `string`.
BEZEL_TEST(cascade_plus_poisoned_operand_7750,
           string_plus_builtin_void_call_is_refused) {
    auto r = runAot(R"(module M;
import System;
class C
  public static method Main(): int
    Console.WriteLine("x=" + Console.WriteLine("a"));
    return 0;
  end Main;
end C;
)");
    expectPlusRejectedOnceAt(r, ":5:23: ");
}

// p20: the same with the void call on the LEFT, into a `var`.
BEZEL_TEST(cascade_plus_poisoned_operand_7750,
           builtin_void_call_plus_string_into_var_is_refused) {
    auto r = runAot(R"(module M;
import System;
class C
  public static method Main(): int
    var s := Console.Write("a") + "b";
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)");
    expectPlusRejectedOnceAt(r, ":5:14: ");
}

// --- an operand poisoned by an ALREADY-REPORTED error: no `+` cascade ---

// The issue's shape: an unresolved type name leaves the local untyped.
BEZEL_TEST(cascade_plus_poisoned_operand_7750,
           unresolved_type_operand_has_no_plus_cascade) {
    auto r = runAot(R"(module M;
import System;
class C
  public static method Main(): int
    var l := new Nope<int>();
    Console.WriteLine("n=" + l.Count.ToString() + "|" + l.Count.ToString());
    return 0;
  end Main;
end C;
)");
    expectNoPlusCascade(r, "unresolved type name `Nope`");
}

// p13: an unknown `string` member reports its own error first.
BEZEL_TEST(cascade_plus_poisoned_operand_7750,
           unknown_string_member_operand_has_no_plus_cascade) {
    auto r = runAot(R"(module M;
import System;
class C
  public static method Main(): int
    var s := "abc";
    Console.WriteLine("x=" + s.Nope);
    return 0;
  end Main;
end C;
)");
    expectNoPlusCascade(r, ":6:30: string member `.Nope`");
}

// p23: an untyped lambda parameter reports its own error first.
BEZEL_TEST(cascade_plus_poisoned_operand_7750,
           untyped_lambda_parameter_operand_has_no_plus_cascade) {
    auto r = runAot(R"(module M;
import System;
class C
  public static method Main(): int
    var f := lambda(x) => "a" + x;
    Console.WriteLine("ok");
    return 0;
  end Main;
end C;
)");
    expectNoPlusCascade(r, "parameter `x` needs a type annotation");
}
