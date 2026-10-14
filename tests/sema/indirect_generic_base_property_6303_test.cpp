// #6303 (§12.1, §11.2, v0.338): a property or indexer override is compared with its base member's type after substituting the base's type arguments THROUGH the whole chain, as a method override is (#6299). `B<T>` declares `property V: T` (or `[i: int]: T`), `D<T>: B<T>` does not redeclare it, and `E: D<int>` overrides with `V: int`: the indirect base's T is int, so the override matches. The direct-base-only substitution left T unbound, so the override was compared with the bare `T` and falsely rejected.
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

// The issue repro (probe p01): `E: D<int>` overrides the indirect base's `property V: T` as `property V: int`. The read through a `D<int>` reference dispatches to the override.
BEZEL_TEST(indirect_generic_base_property_6303, concrete_property_through_indirect_base_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual property V: T => default(T);
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override property V: int => 4;
end E;
public class Program
  public static method Main(): int
    var d: D<int> := new E();
    Console.WriteLine(d.V);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}

// The issue repro (probe p03): the indexer `[i: int]: T` of the indirect base, overridden through `D<int>` as `[i: int]: int`. The read through a `D<int>` reference dispatches to the override.
BEZEL_TEST(indirect_generic_base_property_6303, concrete_indexer_through_indirect_base_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual property [i: int]: T => default(T);
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override property [i: int]: int => i + 40;
end E;
public class Program
  public static method Main(): int
    var d: D<int> := new E();
    Console.WriteLine(d[2]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// The chain's argument is bound one level up: `C<U>: D<U>`, `D<T>: B<T>`, and `E: C<string>` overrides `property V: string`.
BEZEL_TEST(indirect_generic_base_property_6303, three_level_chain_property_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual property V: T => default(T);
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class C<U>: D<U>
  public constructor() end constructor;
end C;
public class E: C<string>
  public constructor() end constructor;
  public override property V: string => "chain";
end E;
public class Program
  public static method Main(): int
    var b: B<string> := new E();
    Console.WriteLine(b.V);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "chain\n");
}

// Rejects: the property's type is the chain's argument `int`, so an override of type `string` does not match.
BEZEL_TEST(indirect_generic_base_property_6303, wrong_property_type_through_indirect_base_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual property V: T => default(T);
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override property V: string => "no";
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's property type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: the indexer's key type is the chain's argument `int`, so a `string` key does not match.
BEZEL_TEST(indirect_generic_base_property_6303, wrong_indexer_key_through_indirect_base_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;
public class B<T>
  public constructor() end constructor;
  public virtual property [i: T]: int => 1;
end B;
public class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E: D<string>
  public constructor() end constructor;
  public override property [i: int]: int => 4;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's index parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
