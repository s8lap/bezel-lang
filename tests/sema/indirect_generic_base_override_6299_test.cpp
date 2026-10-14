// #6299 (§12.1, §11.2, v0.338): an override is compared with its base member's parameter types after substituting the base's type arguments THROUGH the whole chain. `B<T>` declares `P(a: T)`, `D<T>: B<T>` does not redeclare it, and `E1: D<int>` overrides `P(a: int)`: the indirect base's T is int, so the override matches. The direct-base-only substitution left T unbound, so the override was compared with the bare `T` and rejected.
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

// The issue repro: `E1: D<int>` overrides the indirect base's `P(a: T)` as `P(a: int)`. The call through a `D<int>` reference dispatches to the override.
BEZEL_TEST(indirect_generic_base_override_6299, concrete_arg_through_indirect_base_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual method P(a: T): int => 1;
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E1: D<int>
  public constructor() end constructor;
  public override method P(a: int): int => 3;
end E1;
public class Program
  public static method Main(): int
    var d: D<int> := new E1();
    Console.WriteLine(d.P(1));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// The same override through a three-level chain: `C<U>: D<U>`, `D<T>: B<T>`, and `E: C<string>` overriding `P(a: string)`.
BEZEL_TEST(indirect_generic_base_override_6299, three_level_chain_concrete_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual method P(a: T): int => 1;
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class C<U>: D<U>
  public constructor() end constructor;
end C;
public class E: C<string>
  public constructor() end constructor;
  public override method P(a: string): int => 4;
end E;
public class Program
  public static method Main(): int
    var b: B<string> := new E();
    Console.WriteLine(b.P("x"));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}

// Two type parameters permuted along the chain: `D<X,Y>: B<Y,X>` makes B's X the string and B's Y the int of `D<int,string>`, so `P(a: string, b: int)` is the matching override.
BEZEL_TEST(indirect_generic_base_override_6299, two_params_swapped_by_chain_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<X, Y>
  public constructor() end constructor;
  public virtual method P(a: X, b: Y): int => 1;
end B;
public class D<X, Y>: B<Y, X>
  public constructor() end constructor;
end D;
public class E: D<int, string>
  public constructor() end constructor;
  public override method P(a: string, b: int): int => 5;
end E;
public class Program
  public static method Main(): int
    var d: D<int, string> := new E();
    Console.WriteLine(d.P("s", 2));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// A non-generic intermediate class fixes the argument: `D: B<int>`, `E: D` overriding `P(a: int)`.
BEZEL_TEST(indirect_generic_base_override_6299, non_generic_intermediate_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual method P(a: T): int => 1;
end B;
public class D: B<int>
  public constructor() end constructor;
end D;
public class E: D
  public constructor() end constructor;
  public override method P(a: int): int => 6;
end E;
public class Program
  public static method Main(): int
    var b: B<int> := new E();
    Console.WriteLine(b.P(1));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// A parameter typed by an intermediate that wraps T: `D<U>: B<Array<U>>`, `E: D<int>` overriding `P(a: Array<int>)`.
BEZEL_TEST(indirect_generic_base_override_6299, wrapped_type_arg_through_chain_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual method P(a: T): int => 1;
end B;
public class D<U>: B<Array<U>>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override method P(a: Array<int>): int => 7;
end E;
public class Program
  public static method Main(): int
    var b: B<Array<int>> := new E();
    var a: Array<int> := [1, 2];
    Console.WriteLine(b.P(a));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// Rejects: a concrete argument must match exactly. `E1: D<int>` overriding `P(a: string)` is not an override of `P(a: int)`.
BEZEL_TEST(indirect_generic_base_override_6299, wrong_concrete_arg_through_indirect_base_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual method P(a: T): int => 1;
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E1: D<int>
  public constructor() end constructor;
  public override method P(a: string): int => 3;
end E1;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: the chain's argument is the wrapped type, so `P(a: int)` under `D<int>` (whose base takes `Array<int>`) is not an override.
BEZEL_TEST(indirect_generic_base_override_6299, unwrapped_arg_under_wrapping_chain_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual method P(a: T): int => 1;
end B;
public class D<U>: B<Array<U>>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override method P(a: int): int => 7;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's parameter types"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: a swapped two-parameter chain. `D<int,string>` binds B's X to string and Y to int, so `P(a: int, b: string)` is not the override.
BEZEL_TEST(indirect_generic_base_override_6299, two_params_swapped_by_chain_wrong_order_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<X, Y>
  public constructor() end constructor;
  public virtual method P(a: X, b: Y): int => 1;
end B;
public class D<X, Y>: B<Y, X>
  public constructor() end constructor;
end D;
public class E: D<int, string>
  public constructor() end constructor;
  public override method P(a: int, b: string): int => 5;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's parameter types"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: `P<V>(a: V, b: V)` against the base's `P<U>(a: T, b: U)` with T := int. The first parameter is `int` in the base and `V` here, so it mismatches.
BEZEL_TEST(indirect_generic_base_override_6299, own_type_param_through_chain_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual method P<U>(a: T, b: U): int => 1;
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override method P<V>(a: V, b: V): int => 8;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's parameter types"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Accept: the own type parameter corresponds positionally through the chain, and the class argument is concrete. Called through an `E` reference: a base-typed call of a generic-method override does not dispatch to it (#6300, a separate codegen defect present with a direct base too).
BEZEL_TEST(indirect_generic_base_override_6299, own_type_param_through_chain_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual method P<U>(a: T, b: U): int => 1;
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override method P<V>(a: int, b: V): int => 9;
end E;
public class Program
  public static method Main(): int
    var e: E := new E();
    Console.WriteLine(e.P<string>(1, "x"));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}
