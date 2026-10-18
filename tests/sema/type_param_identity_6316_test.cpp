// #6316 (§12.1, §11.2, §9.1, v0.338): residuals of #6315. A class type parameter is a declaration-less Named type whose only identity is its spelling, so the blind typeRefEqual equated any two of them. (a) The abstract/virtual indexer or property VALUE type of an override (`[i: K]: W` implemented by `[i: P]: P` in `E<P, Q>: B2<P, Q>`) and (c) the property type of a NON-generic interface implemented by a class property, an inherited property or a record positional field (`Box<Exception>` vs `Box<U>`) now compare the two sides, both read in the implementing class's scope, by type-parameter spelling. The override PARAMETER compare (overrideParamTypeMatches) is a separate follow-up.
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

const char* kOverrideMsg = "does not match the base member's property type";
const char* kIfaceMsg = "implements interface `N` with a";

}  // namespace

BEZEL_TEST(type_param_identity_6316, abstract_indexer_value_bare_other_parameter_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public class E<P, Q>: B2<P, Q>
  public constructor() end constructor;
  public override property [i: P]: P => i;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, abstract_indexer_value_nested_other_parameter_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public class E<P, Q>: B2<P, Array<Q>?>
  public constructor() end constructor;
  public override property [i: P]: Array<P>? => null;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, abstract_indexer_key_and_value_wrong_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public class E<P, Q>: B2<P, Q>
  public constructor() end constructor;
  public override property [i: Q]: P => i;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not implement inherited abstract member `indexer`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, abstract_property_value_other_parameter_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property Val: W;
end B2;
public class E<P, Q>: B2<P, Q>
  public constructor() end constructor;
  public override property Val: P => default(P);
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, abstract_indexer_value_through_chain_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public abstract class D<X, Y>: B2<X, Y>
  public constructor() end constructor;
end D;
public class E<P, Q>: D<P, Q>
  public constructor() end constructor;
  public override property [i: P]: P => i;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, virtual_indexer_value_other_parameter_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public class V2<K, W>
  public constructor() end constructor;
  public virtual property [i: K]: W => default(W);
end V2;
public class E<P, Q>: V2<P, Q>
  public constructor() end constructor;
  public override property [i: P]: P => i;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, abstract_property_value_swapped_chain_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property Val: W;
end B2;
public abstract class D<X, Y>: B2<Y, X>
  public constructor() end constructor;
end D;
public class E<P, Q>: D<P, Q>
  public constructor() end constructor;
  public override property Val: Q => default(Q);
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, iface_property_nonnull_generic_arg_other_parameter_rejected_spec_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public class E<U>: N
  public constructor() end constructor;
  public property Val: Box<U>? => null;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kIfaceMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, iface_property_bare_builtin_vs_parameter_rejected_spec_11_2) {
    auto r = runAot(R"BZ(
module M;
public interface N
  readonly property Val: Exception?;
end N;
public class E<U>: N
  public constructor() end constructor;
  public property Val: U? => null;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kIfaceMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, iface_property_positional_field_other_parameter_rejected_spec_9_1) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public record R<U>(Val: Box<U>?): N end R;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kIfaceMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, iface_property_inherited_generic_base_rejected_spec_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public class Bs<T>
  public constructor() end constructor;
  public property Val: Box<T>? => null;
end Bs;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public class K<U>: Bs<U>, N
  public constructor() end constructor;
end K;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kIfaceMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, abstract_property_builtin_arg_vs_parameter_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public abstract class B3
  public constructor() end constructor;
  public abstract property Val: Box<Exception>?;
end B3;
public class E<U>: B3
  public constructor() end constructor;
  public override property Val: Box<U>? => null;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, iface_property_two_level_generic_base_rejected_spec_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public class Bs<T>
  public constructor() end constructor;
  public property Val: Box<T>? => null;
end Bs;
public class Mid<V>: Bs<V>
  public constructor() end constructor;
end Mid;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public class K<U>: Mid<U>, N
  public constructor() end constructor;
end K;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kIfaceMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, virtual_chain_override_value_rejected_spec_12_1) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public abstract class D<X, Y>: B2<X, Y>
  public constructor() end constructor;
  public override property [i: X]: Y => default(Y);
end D;
public class E<P, Q>: D<P, Q>
  public constructor() end constructor;
  public override property [i: P]: P => i;
end E;
public class Program
  public static method Main(): int

    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_identity_6316, accepted_a01_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public class E<P, Q>: B2<P, Q>
  public constructor() end constructor;
  public override property [i: P]: Q => default(Q);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e[3] + 1);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a02_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public class E<P, Q>: B2<P, Array<Q>?>
  public constructor() end constructor;
  public override property [i: P]: Array<Q>? => null;
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(2);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a04_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public abstract class D<X, Y>: B2<X, Y>
  public constructor() end constructor;
end D;
public class E<P, Q>: D<P, Q>
  public constructor() end constructor;
  public override property [i: P]: Q => default(Q);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e[3] + 4);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a05_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public abstract class D<X, Y>: B2<Y, X>
  public constructor() end constructor;
end D;
public class E<P, Q>: D<P, Q>
  public constructor() end constructor;
  public override property [i: Q]: P => default(P);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e[3] + 5);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a06_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public class E<K, W>: B2<W, K>
  public constructor() end constructor;
  public override property [i: W]: K => default(K);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e[3] + 6);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a07_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property [i: int]: T;
end B;
public class E<T>: B<Array<T>?>
  public constructor() end constructor;
  public override property [i: int]: Array<T>? => null;
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    Console.WriteLine(7);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a08_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B<T>
  public constructor() end constructor;
  public abstract property Val: T;
end B;
public class E<T>: B<T>
  public constructor() end constructor;
  public override property Val: T => default(T);
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    Console.WriteLine(e.Val + 8);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "8\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a16_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public class V2<K, W>
  public constructor() end constructor;
  public virtual property [i: K]: W => default(W);
end V2;
public class E<P, Q>: V2<P, Q>
  public constructor() end constructor;
  public override property [i: P]: Q => default(Q);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e[2] + 16);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "16\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a22_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property Val: W;
end B2;
public abstract class D<X, Y>: B2<X, Y>
  public constructor() end constructor;
end D;
public abstract class F<M, N>: D<M, N>
  public constructor() end constructor;
end F;
public class E<P, Q>: F<P, Q>
  public constructor() end constructor;
  public override property Val: Q => default(Q);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e.Val + 22);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "22\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a27_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property Val: W;
end B2;
public abstract class D<X, Y>: B2<Y, X>
  public constructor() end constructor;
end D;
public class E<P, Q>: D<P, Q>
  public constructor() end constructor;
  public override property Val: P => default(P);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e.Val + 27);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "27\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a31_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public class E<U>: N
  public constructor() end constructor;
  public property Val: Box<Exception>? => null;
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    Console.WriteLine(41);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "41\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a32_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public interface N
  readonly property Val: Exception?;
end N;
public class E<U>: N
  public constructor() end constructor;
  public property Val: Exception? => null;
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    Console.WriteLine(42);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a33_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public record R(Val: Box<Exception>?): N end R;
public class Program
  public static method Main(): int
    var r := new R(null);
    Console.WriteLine(43);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "43\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a34_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public class Bs<T>
  public constructor() end constructor;
  public property Val: Box<T>? => null;
end Bs;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public class K: Bs<Exception>, N
  public constructor() end constructor;
end K;
public class Program
  public static method Main(): int
    var e := new K();
    Console.WriteLine(44);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "44\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a35_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public abstract class B3
  public constructor() end constructor;
  public abstract property Val: Box<Exception>?;
end B3;
public class E<U>: B3
  public constructor() end constructor;
  public override property Val: Box<Exception>? => null;
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    Console.WriteLine(45);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "45\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a38_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public class Bs<T>
  public constructor() end constructor;
  public property Val: Box<T>? => null;
end Bs;
public class Mid: Bs<Exception>
  public constructor() end constructor;
end Mid;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public class K: Mid, N
  public constructor() end constructor;
end K;
public class Program
  public static method Main(): int
    var e := new K();
    Console.WriteLine(48);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "48\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a39_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public class Bs<T>
  public constructor() end constructor;
  public property Val: Box<T>? => null;
end Bs;
public class Mid<V>: Bs<V>
  public constructor() end constructor;
end Mid;
public interface N
  readonly property Val: Box<Exception>?;
end N;
public class K<U>: Mid<Exception>, N
  public constructor() end constructor;
end K;
public class Program
  public static method Main(): int
    var e := new K<int>();
    Console.WriteLine(49);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "49\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a40_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public abstract class D<X, Y>: B2<X, Y>
  public constructor() end constructor;
  public override property [i: X]: Y => default(Y);
end D;
public class E<P, Q>: D<P, Q>
  public constructor() end constructor;
  public override property [i: P]: Q => default(Q);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e[3] + 50);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "50\n");
}

BEZEL_TEST(type_param_identity_6316, accepted_a41_spec_12_1_11_2) {
    auto r = runAot(R"BZ(
module M;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public abstract class D<X, Y>: B2<Y, X>
  public constructor() end constructor;
  public override property [i: Y]: X => default(X);
end D;
public class E<P, Q>: D<P, Q>
  public constructor() end constructor;
  public override property [i: Q]: P => default(P);
end E;
public class Program
  public static method Main(): int
    var e := new E<int, int>();
    Console.WriteLine(e[3] + 51);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "51\n");
}
