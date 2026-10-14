// #6308 (§12.1, §12.5, §11.2, v0.338): an abstract indexer whose key type is a base class's generic parameter `T` is implemented by an override whose key is the concrete type the subclass binds `T` to. The obligation's key types are read through the chain substitution its visit induced (`B<T>` under `E: B<int>` binds T to int), as the override-signature check already does; the candidate search compared the raw `T` against `int` and falsely rejected the valid override. The same shape with a virtual indexer was accepted, which pins the target behaviour.
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

// The issue repro with a runnable key: `B<T>` declares `abstract property [i: T]: int`, `E: B<int>` overrides it as `[i: int]: int`. The read through a `B<int>` reference dispatches to the override.
BEZEL_TEST(abstract_indexer_generic_key_6308, concrete_key_implements_abstract_generic_key_accepted_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class E: B<int>
  public constructor() end constructor;
  public override property [i: int]: int => i + 40;
end E;
public class Program
  public static method Main(): int
    var b: B<int> := new E();
    Console.WriteLine(b[2]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// Two indexer parameters: only the first is generic. The second parameter's concrete type is compared as written.
BEZEL_TEST(abstract_indexer_generic_key_6308, multi_parameter_generic_key_accepted_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T, j: int]: int;
end B;
public class E: B<int>
  public constructor() end constructor;
  public override property [i: int, j: int]: int => i * 10 + j;
end E;
public class Program
  public static method Main(): int
    var b: B<int> := new E();
    Console.WriteLine(b[4, 2]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// The chain binds the key one level up: `D<T>: B<T>`, `E: D<int>`.
BEZEL_TEST(abstract_indexer_generic_key_6308, three_level_chain_generic_key_accepted_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public abstract class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override property [i: int]: int => i + 1;
end E;
public class Program
  public static method Main(): int
    var b: B<int> := new E();
    Console.WriteLine(b[41]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// Swapped parameters: `B<X, Y>` keys by `Y`, so `E: B<string, int>` implements the key with `int`, not the first argument.
BEZEL_TEST(abstract_indexer_generic_key_6308, swapped_type_arguments_key_is_second_accepted_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<X, Y>
  public constructor() end constructor;
  public abstract property [i: Y]: int;
end B;
public class E: B<string, int>
  public constructor() end constructor;
  public override property [i: int]: int => i + 1;
end E;
public class Program
  public static method Main(): int
    var b: B<string, int> := new E();
    Console.WriteLine(b[5]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// An interface's generic-keyed read-only indexer is implemented by a class with the bound key type (the same candidate search, obligation shape of an interface).
BEZEL_TEST(abstract_indexer_generic_key_6308, interface_generic_key_accepted_spec_11_2) {
    auto r = runAot(R"(
module M;
public interface I<T>
  readonly property [i: T]: int;
end I;
public class C: I<int>
  public constructor() end constructor;
  public property [i: int]: int => i + 5;
end C;
public class Program
  public static method Main(): int
    var x: I<int> := new C();
    Console.WriteLine(x[1]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// Guard row: the candidate is an override in a generic ancestor `N<U>` whose key is `U`; `E: N<int>` inherits the candidate, and the obligation key `T` is read as int through the chain. It also passes before the fix, so it guards the accept path rather than pinning this fix.
BEZEL_TEST(abstract_indexer_generic_key_6308, candidate_in_generic_ancestor_accepted_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public abstract class N<U>: B<U>
  public constructor() end constructor;
  public override property [i: U]: int => 7;
end N;
public class E: N<int>
  public constructor() end constructor;
end E;
public class Program
  public static method Main(): int
    var b: B<int> := new E();
    Console.WriteLine(b[0]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// Rejects: the key type is still compared after substitution, so an override keyed by `string` does not implement `B<int>`'s `[i: T]`.
BEZEL_TEST(abstract_indexer_generic_key_6308, wrong_key_type_direct_base_rejected_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class E: B<int>
  public constructor() end constructor;
  public override property [i: string]: int => 1;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: the reverse direction, `B<string>`'s key is `string`, an override keyed by `int` does not match.
BEZEL_TEST(abstract_indexer_generic_key_6308, wrong_key_type_reverse_rejected_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class E: B<string>
  public constructor() end constructor;
  public override property [i: int]: int => 1;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: a second, non-generic key parameter is still compared as written.
BEZEL_TEST(abstract_indexer_generic_key_6308, multi_parameter_second_key_mismatch_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T, j: int]: int;
end B;
public class E: B<int>
  public constructor() end constructor;
  public override property [i: int, j: string]: int => 1;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: the chain's key is `int`, so a `string` override key fails through the two-level chain.
BEZEL_TEST(abstract_indexer_generic_key_6308, wrong_key_through_three_level_chain_rejected_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public abstract class D<T>: B<T>
  public constructor() end constructor;
end D;
public class E: D<int>
  public constructor() end constructor;
  public override property [i: string]: int => 1;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: an interface's generic key `T = int` is not implemented by a `string`-keyed indexer.
BEZEL_TEST(abstract_indexer_generic_key_6308, interface_generic_key_mismatch_rejected_spec_11_2) {
    auto r = runAot(R"(
module M;
public interface I<T>
  readonly property [i: T]: int;
end I;
public class C: I<int>
  public constructor() end constructor;
  public property [i: string]: int => 5;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// Rejects: an override in `N<U>` keyed by `int` does not implement `B<U>` for `E: N<string>`.
BEZEL_TEST(abstract_indexer_generic_key_6308, candidate_in_generic_ancestor_key_mismatch_rejected_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public abstract class N<U>: B<U>
  public constructor() end constructor;
  public override property [i: int]: int => 7;
end N;
public class E: N<string>
  public constructor() end constructor;
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}
