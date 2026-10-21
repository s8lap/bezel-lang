// fix #6791 (§31.1, §3.6 rule 2, §14.2, v0.340): `throw x` on a bare type
// parameter whose class bound is `Exception` (or a class deriving from it) is
// accepted. §31.1: "the operand's static type, nullability not admitted, must
// be `Exception` or a class deriving from it". §3.6 rule 2: "A generic body
// type-checks under the weakest assumption its constraints establish", and
// §14.2's class-bound row admits "Non-nullable subtypes of `SomeClass` (the
// class itself and its descendants)". Every admitted substitution is
// throwable, so the bound decides. An unbounded `T`, a `class`, interface or
// non-exception class bound, and a nullable operand that is not narrowed
// remain rejected.
//
// Probes: /var/tmp/bezel-scratch/r34-diag/p/t3_*.bzl.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kAppError =
    "public class AppError: Exception\n"
    "  public constructor(m: string) base(m); end constructor;\n"
    "end AppError;\n"
    "public class SubError: AppError\n"
    "  public constructor(m: string) base(m); end constructor;\n"
    "end SubError;\n";

}  // namespace

// The issue's W4 repro (`where T: Exception`), run: the thrown exception is
// caught and its message printed.
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           where_exception_bound_throw_checked_spec_31_1) {
    auto r = runAotChecked(R"(
module Conf;
public class Program
  public static method M<T>(owned x: T) where T: Exception
    throw x;
  end M;
  public static method Main()
    try
      M(new IOException("m"));
    catch
      var e: Exception => Console.WriteLine(e.Message)
    end try;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("m\n"));
}

// The issue's W5 repro (a user class bound), caught as the bound class and as
// the more derived class.
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           where_user_exception_class_bound_throw_checked_spec_31_1) {
    auto r = runAotChecked(std::string("module Conf;\n") + kAppError + R"(
public class Program
  public static method M<T>(owned x: T) where T: AppError
    throw x;
  end M;
  public static method Main()
    try
      M(new AppError("m2"));
    catch
      var e: AppError => Console.WriteLine($"A:{e.Message}")
    end try;
    try
      M(new SubError("m3"));
    catch
      SubError => Console.WriteLine("sub"),
      var e: AppError => Console.WriteLine($"A:{e.Message}")
    end try;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A:m2\nsub\n"));
}

// §14.2 `where T: U` chain ending in an `Exception` bound.
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           where_type_param_chain_throw_checked_spec_14_2) {
    auto r = runAotChecked(R"(
module Conf;
public class Program
  public static method M<T, U>(owned x: T) where T: U where U: Exception
    throw x;
  end M;
  public static method Main()
    try
      M<IOException, Exception>(new IOException("chain"));
    catch
      var e: Exception => Console.WriteLine(e.Message)
    end try;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("chain\n"));
}

// The bound on the enclosing generic TYPE decides the same way.
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           generic_type_bound_throw_checked_spec_31_1) {
    auto r = runAotChecked(R"(
module Conf;
public class Thrower<T> where T: Exception
  public method Go(owned x: T)
    throw x;
  end Go;
end Thrower;
public class Program
  public static method Main()
    var t := new Thrower<ArgumentException>();
    try
      t.Go(new ArgumentException("gt"));
    catch
      var e: Exception => Console.WriteLine(e.Message)
    end try;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("gt\n"));
}

// A `T?` operand keeps §31.1's nullability rule: narrowed by `if x <> null`
// it is throwable.
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           nullable_bound_param_narrowed_throw_checked_spec_31_1) {
    auto r = runAotChecked(R"(
module Conf;
public class Program
  public static method M<T>(owned x: T?) where T: Exception
    if x <> null then
      throw x;
    end if;
  end M;
  public static method Main()
    try
      M<IOException>(new IOException("nn"));
    catch
      var e: Exception => Console.WriteLine(e.Message)
    end try;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nn\n"));
}

// Disposal: each thrown instance is released by the catch arm. 300 iterations
// so a per-throw leak or double release shows under the sanitizer.
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           repeated_bound_throw_no_leak_spec_31_1) {
    auto r = runAotChecked(std::string("module Conf;\n") + kAppError + R"(
public class Program
  public static method M<T>(owned x: T) where T: AppError
    throw x;
  end M;
  private static _total: int := 0;
  public static method Record(n: int)
    _total := _total + n;
  end Record;
  public static method Count()
    try
      M(new AppError("e"));
    catch
      var e: AppError => Record(e.Message.Length)
    end try;
  end Count;
  public static method Main(): int
    for var i in 0..299 do
      Count();
    end for;
    Console.WriteLine($"{_total}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

// Still rejected: an unbounded `T` (the pinned text of #3046 is unchanged).
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           unbounded_param_still_rejected_spec_31_1) {
    auto r = runAot(R"(
module Conf;
public class Program
  public static method M<T>(owned x: T)
    throw x;
  end M;
  public static method Main()
    M(new IOException("m"));
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "the type parameter `T` carries no `Exception`-derived class bound"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Still rejected: `class`, an interface bound, and a user class that does not
// derive from `Exception`.
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           non_exception_bounds_still_rejected_spec_31_1) {
    auto c = runAot(R"(
module Conf;
public class Program
  public static method M<T>(owned x: T) where T: class
    throw x;
  end M;
  public static method Main()
    M(new IOException("m"));
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(c, "carries no `Exception`-derived"));
    auto i = runAot(R"(
module Conf;
public class Program
  public static method M<T>(owned x: T) where T: IComparable<T>
    throw x;
  end M;
  public static method Main()
    M(3);
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(i, "carries no `Exception`-derived"));
    auto p = runAot(R"(
module Conf;
public class Plain
  public constructor() end constructor;
end Plain;
public class Program
  public static method M<T>(owned x: T) where T: Plain
    throw x;
  end M;
  public static method Main()
    M(new Plain());
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(p, "carries no `Exception`-derived"));
    BEZEL_EXPECT_FALSE(p.ran);
}

// Still rejected: a `T?` operand with no narrowing fact (§31.1 "nullability
// not admitted").
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           nullable_bound_param_unnarrowed_still_rejected_spec_31_1) {
    auto r = runAot(R"(
module Conf;
public class Program
  public static method M<T>(owned x: T?) where T: Exception
    throw x;
  end M;
  public static method Main()
    M<IOException>(null);
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "the throw operand's type is nullable"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Still rejected: a BORROWED `x: T` (§31.1's ownership rule is independent of
// the type rule; the owned diagnostic now fires alone).
BEZEL_TEST(throw_bare_typeparam_exception_bound_6791,
           borrowed_bound_param_still_rejected_owned_spec_31_1) {
    auto r = runAot(R"(
module Conf;
public class Program
  public static method M<T>(x: T) where T: Exception
    throw x;
  end M;
  public static method Main()
    M(new IOException("m"));
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "must be an OWNED exception"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "carries no `Exception`-derived"));
    BEZEL_EXPECT_FALSE(r.ran);
}
