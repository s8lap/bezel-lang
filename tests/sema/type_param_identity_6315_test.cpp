// #6315 (§11.2, §12.1, §12.5, v0.338): a class type parameter is a declaration-less Named type whose only identity is its spelling, so the declaration-pointer compare equated any two of them (`Array<U>` with `Array<V>`). The abstract/interface implementation checks now compare the two sides, both read in the implementing class's scope after substitution, with a scope-aware equality: an abstract or interface indexer keyed by `U` is not implemented by a key of `V`, bare or nested. The override-signature sites (overrideParamTypeMatches and the override return/property sites) are a separate follow-up.
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

const char* kMain = R"(
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)";

}  // namespace

// `B<U>`'s key is `U`; an override keyed by the class's other parameter `V` is not it.
BEZEL_TEST(type_param_identity_6315, abstract_indexer_bare_other_parameter_rejected_spec_12_5) {
    auto r = runAot(std::string(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class G<U, V>: B<U>
  public constructor() end constructor;
  public override property [i: V]: int => 1;
end G;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `B<Array<U>>`'s key is `Array<U>`; `Array<V>` is not it.
BEZEL_TEST(type_param_identity_6315, abstract_indexer_nested_other_parameter_rejected_spec_12_5) {
    auto r = runAot(std::string(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class G<U, V>: B<Array<U>>
  public constructor() end constructor;
  public override property [i: Array<V>]: int => 1;
end G;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `B<(U) -> V>`'s key is `(U) -> V`; `(V) -> U` is not it.
BEZEL_TEST(type_param_identity_6315, abstract_indexer_function_key_swapped_rejected_spec_12_5) {
    auto r = runAot(std::string(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class G<U, V>: B<(U) -> V>
  public constructor() end constructor;
  public override property [i: (V) -> U]: int => 1;
end G;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `B<Box<U>>`'s key is `Box<U>` (a declared generic class); `Box<V>` is not it.
BEZEL_TEST(type_param_identity_6315, abstract_indexer_user_generic_key_rejected_spec_12_5) {
    auto r = runAot(std::string(R"(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class G<U, V>: B<Box<U>>
  public constructor() end constructor;
  public override property [i: Box<V>]: int => 1;
end G;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `B<U?>`'s key is `U?`; `V?` is not it.
BEZEL_TEST(type_param_identity_6315, abstract_indexer_nullable_other_parameter_rejected_spec_12_5) {
    auto r = runAot(std::string(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class G<U, V>: B<U?>
  public constructor() end constructor;
  public override property [i: V?]: int => 1;
end G;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `I<U>`'s key is `U`; an implementing indexer keyed by `V` is not it.
BEZEL_TEST(type_param_identity_6315, interface_indexer_bare_other_parameter_rejected_spec_11_2) {
    auto r = runAot(std::string(R"(
module M;
public interface I<T>
  readonly property [i: T]: int;
end I;
public class C<U, V>: I<U>
  public constructor() end constructor;
  public property [i: V]: int => 1;
end C;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement interface `I`'s indexer"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `I<Array<U>>`'s key is `Array<U>`; `Array<V>` is not it.
BEZEL_TEST(type_param_identity_6315, interface_indexer_nested_other_parameter_rejected_spec_11_2) {
    auto r = runAot(std::string(R"(
module M;
public interface I<T>
  readonly property [i: T]: int;
end I;
public class C<U, V>: I<Array<U>>
  public constructor() end constructor;
  public property [i: Array<V>]: int => 1;
end C;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement interface `I`'s indexer"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `I<Dictionary<U, V>>`'s key is `Dictionary<U, V>`; the swapped `Dictionary<V, U>` is not it.
BEZEL_TEST(type_param_identity_6315, interface_indexer_dictionary_swapped_rejected_spec_11_2) {
    auto r = runAot(std::string(R"(
module M;
public interface I<T>
  readonly property [i: T]: int;
end I;
public class C<U, V>: I<Dictionary<U, V>>
  public constructor() end constructor;
  public property [i: Dictionary<V, U>]: int => 1;
end C;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement interface `I`'s indexer"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `I<Box<U>>`'s `Val` is `Box<U>` (a declared generic class); a property typed `Box<V>` is not it.
BEZEL_TEST(type_param_identity_6315, interface_property_user_generic_type_rejected_spec_11_2) {
    auto r = runAot(std::string(R"(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public interface I<T>
  readonly property Val: T;
end I;
public class C<U, V>: I<Box<U>>
  public constructor() end constructor;
  public property Val: Box<V> => throw new InvalidOperationException("n");
end C;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "implements interface `I` with a different type than the interface declares"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `I<(U) -> V>`'s `Val` is `(U) -> V`; `(V) -> U` is not it.
BEZEL_TEST(type_param_identity_6315, interface_property_function_type_swapped_rejected_spec_11_2) {
    auto r = runAot(std::string(R"(
module M;
public interface I<T>
  readonly property Val: T;
end I;
public class C<U, V>: I<(U) -> V>
  public constructor() end constructor;
  public property Val: (V) -> U => throw new InvalidOperationException("n");
end C;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "implements interface `I` with a different type than the interface declares"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `I<Array<U>>`'s `Val` is `Array<U>`; `Array<V>` is not it.
BEZEL_TEST(type_param_identity_6315, interface_property_array_other_parameter_rejected_spec_11_2) {
    auto r = runAot(std::string(R"(
module M;
public interface I<T>
  readonly property Val: T;
end I;
public class C<U, V>: I<Array<U>>
  public constructor() end constructor;
  public property Val: Array<V> => throw new InvalidOperationException("n");
end C;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "implements interface `I` with a different type than the interface declares"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control: the override keyed by the SAME parameter `U` implements `B<U>`'s key.
BEZEL_TEST(type_param_identity_6315, abstract_indexer_same_parameter_accepted_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class G<U, V>: B<U>
  public constructor() end constructor;
  public override property [i: U]: int => 7;
end G;
public class Program
  public static method Main(): int
    var b: B<int> := new G<int, string>();
    Console.WriteLine(b[3]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// Control: `Array<U>` is implemented by `Array<U>` (compile-only: reading a reference-typed key through an indexer is a separate codegen defect noted in #6308).
BEZEL_TEST(type_param_identity_6315, abstract_indexer_nested_same_parameter_accepted_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public class G<U, V>: B<Array<U>>
  public constructor() end constructor;
  public override property [i: Array<U>]: int => 7;
end G;
public class Program
  public static method Main(): int
    var g := new G<int, string>();
    Console.WriteLine(7);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// `D<X>: B<X>`, `E<Z>: D<Z>`: the same spelling in unrelated classes denotes different parameters, and each side is read in `E`'s scope after substitution, so `[i: Z]` still implements `B<Z>`'s key.
BEZEL_TEST(type_param_identity_6315, abstract_indexer_renamed_chain_accepted_spec_12_5) {
    auto r = runAot(R"(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: T]: int;
end B;
public abstract class D<X>: B<X>
  public constructor() end constructor;
end D;
public class E<Z>: D<Z>
  public constructor() end constructor;
  public override property [i: Z]: int => 7;
end E;
public class Program
  public static method Main(): int
    var b: B<int> := new E<int>();
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

// Control: `I<U>` is implemented by an indexer keyed by `U`.
BEZEL_TEST(type_param_identity_6315, interface_indexer_same_parameter_accepted_spec_11_2) {
    auto r = runAot(R"(
module M;
public interface I<T>
  readonly property [i: T]: int;
end I;
public class C<U, V>: I<U>
  public constructor() end constructor;
  public property [i: U]: int => 5;
end C;
public class Program
  public static method Main(): int
    var c := new C<int, string>();
    var i: I<int> := c;
    Console.WriteLine(i[3]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}
