// #6039 (§12.1, v0.338): an override may declare `T?` where the virtual declares `T` (the one admitted widening). A compiler-known class (the built-in exceptions, Stack, ...) has no sema declaration, and was rejected as "not ABI-safe" while a user class with the same shape was accepted (a false reject).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// The aot driver reports the position in the message ("<path>:<line>:<col>: <msg>"); the structured location is unset on this path. Pin the needle AND the exact override-method position.
bool hasErrorAt(const JitResult& r, const std::string& needle, unsigned line, unsigned col) {
    const std::string at = ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(at + "override of") != std::string::npos &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// #6039 issue repro: `virtual Log(e: Exception)` widened to `override Log(e: Exception?)` is admitted by §12.1 and dispatches through the base-typed receiver.
BEZEL_TEST(override_widening_builtin_class_6039, exception_widened_to_nullable_spec_12_1_issue_repro) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Exception): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: Exception?): int => 2;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var b: B := c;
    using var x := new Exception("x");
    Console.WriteLine(b.Log(x));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// §12.1 widening for a compiler-known generic class: `Stack<int>` widened to `Stack<int>?`.
BEZEL_TEST(override_widening_builtin_class_6039, stack_widened_to_nullable_spec_12_1) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public virtual method Log(s: Stack<int>): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(s: Stack<int>?): int => 2;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var b: B := c;
    var s := new Stack<int>();
    Console.WriteLine(b.Log(s));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// The widening does not admit a different built-in class: `ArgumentException?` over `Exception` is still a mismatch (§12.1 "match the base member exactly" aside from `T` -> `T?`).
BEZEL_TEST(override_widening_builtin_class_6039, different_builtin_class_widened_still_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Exception): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: ArgumentException?): int => 2;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "does not match the base member's parameter types", 10, 3));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The widening is only `T` -> `T?`: a generic argument that differs under the `?` is still a mismatch.
BEZEL_TEST(override_widening_builtin_class_6039, generic_argument_widened_still_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Array<Exception>): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: Array<ArgumentException>?): int => 2;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "does not match the base member's parameter types", 10, 3));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
