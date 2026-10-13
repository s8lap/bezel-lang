// #6000 (§12.1, v0.338): an override's parameter types must match the base member EXACTLY. A compiler-known class (the built-in exceptions, Stack, Queue, ...) has no declaration in sema, and two such types compared equal whatever their names (override half only; interface conformance is #6000's other half).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// #6000 class half: `virtual Log(e: Exception)` / `override Log(e: ArgumentException)`; the interface call used to run the narrower body.
BEZEL_TEST(override_builtin_class_param_6000, argument_exception_over_exception_spec_12_1_issue_repro) {
    auto r = runAot(R"(
module M;
type EAlias := Exception;
type AEAlias := ArgumentException;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Exception): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: ArgumentException): int => 2;
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// alias of a DIFFERENT built-in class is that class (§27.8).
BEZEL_TEST(override_builtin_class_param_6000, alias_of_other_builtin_spec_27_8) {
    auto r = runAot(R"(
module M;
type EAlias := Exception;
type AEAlias := ArgumentException;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Exception): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: AEAlias): int => 2;
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// same shape, different compiler-known class.
BEZEL_TEST(override_builtin_class_param_6000, stack_over_queue_spec_12_1) {
    auto r = runAot(R"(
module M;
type EAlias := Exception;
type AEAlias := ArgumentException;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Stack<int>): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: Queue<int>): int => 2;
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// the difference is inside a generic argument.
BEZEL_TEST(override_builtin_class_param_6000, generic_argument_exception_spec_12_1) {
    auto r = runAot(R"(
module M;
type EAlias := Exception;
type AEAlias := ArgumentException;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Array<Exception>): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: Array<ArgumentException>): int => 2;
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// control: identical built-in class parameter; the override runs through the base-typed receiver.
BEZEL_TEST(override_builtin_class_param_6000, same_builtin_class_dispatches_spec_12_1) {
    auto r = runAot(R"(
module M;
type EAlias := Exception;
type AEAlias := ArgumentException;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Exception): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: Exception): int => 2;
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

// §27.8: `EAlias` is `Exception`; must not be a false reject.
BEZEL_TEST(override_builtin_class_param_6000, alias_of_same_builtin_class_is_the_same_type_spec_27_8) {
    auto r = runAot(R"(
module M;
type EAlias := Exception;
type AEAlias := ArgumentException;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Exception): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: EAlias): int => 2;
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

// a method-level type parameter renamed by position (§11.2 Generic members) is the same signature.
BEZEL_TEST(override_builtin_class_param_6000, method_generic_param_renamed_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B
  public constructor() end constructor;
  public virtual method Q<T>(x: T): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Q<U>(x: U): int => 2;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var b: B := c;
    Console.WriteLine(b.Q(5));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// 3-level hierarchy with the same built-in parameter at every level.
BEZEL_TEST(override_builtin_class_param_6000, three_level_builtin_param_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B
  public constructor() end constructor;
  public virtual method Log(e: Exception): int => 1;
end B;
public class Mid: B
  public constructor() end constructor;
  public override method Log(e: Exception): int => 2;
end Mid;
public class C: Mid
  public constructor() end constructor;
  public override method Log(e: Exception): int => 3;
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
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// user-class control (always compared by declaration).
BEZEL_TEST(override_builtin_class_param_6000, user_class_param_unchanged_spec_12_1) {
    auto r = runAot(R"(
module M;
public class Animal
  public constructor() end constructor;
end Animal;
public class B
  public constructor() end constructor;
  public virtual method Feed(a: Animal): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Feed(a: Animal): int => 2;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var b: B := c;
    using var a := new Animal();
    Console.WriteLine(b.Feed(a));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}
