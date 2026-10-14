// #6315 (§11.2, §12.1, §12.5, v0.338): a class type parameter is a declaration-less Named type whose only identity is its spelling, so the declaration-pointer compare equated any two of them (`Array<U>` with `Array<V>`). The abstract/interface implementation checks now compare the two sides, both read in the implementing class's scope after substitution, with a scope-aware equality: an abstract or interface indexer keyed by `U` is not implemented by a key of `V`, bare or nested. #6316 (b) extends the rule to the override-signature checks (method parameter, indexer key and return type; rows at the end of this file. The override PROPERTY-type compare is #6316 (a), fixed separately): a base class parameter is compared with its substituted argument at every depth, a method's own type parameters by position.
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

// ---- #6316 (b): the override-signature sites (§12.1, v0.338) ----
// §12.1: "Visibility, return type, parameter types, and parameter modifiers must match the
// base member exactly" ... "An override declares exactly as many type parameters of its own as
// the overridden member ..., the two lists corresponding by position as in §11.2's *Generic
// members*". §11.2: "A member's own type parameters take part in the match by number and
// position, not by name." Each row below was accepted before (typeRefEqual equates two class
// type parameters) and is rejected now.

// `VB<Array<U>>`'s `M` takes `Array<U>`; an override taking `Array<V>` is not it (#6316 (b)).
BEZEL_TEST(type_param_identity_6315, override_param_nested_other_parameter_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
end VB;
public class E<U, V>: VB<Array<U>>
  public constructor() end constructor;
  public override method M(x: Array<V>): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `Array<Array<U>>` against `Array<Array<V>>`.
BEZEL_TEST(type_param_identity_6315, override_param_doubly_nested_other_parameter_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
end VB;
public class E<U, V>: VB<Array<Array<U>>>
  public constructor() end constructor;
  public override method M(x: Array<Array<V>>): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `VB<(U) -> V>`: `(V) -> U` is not `(U) -> V` (the base's `T` substitutes to a function type).
BEZEL_TEST(type_param_identity_6315, override_param_function_type_swapped_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
end VB;
public class E<U, V>: VB<(U) -> V>
  public constructor() end constructor;
  public override method M(x: (V) -> U): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `Dictionary<V, U>` is not `Dictionary<U, V>`.
BEZEL_TEST(type_param_identity_6315, override_param_dictionary_swapped_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
end VB;
public class E<U, V>: VB<Dictionary<U, V>>
  public constructor() end constructor;
  public override method M(x: Dictionary<V, U>): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The §12.1 `T` to `T?` widening admits `Array<U>?`, never `Array<V>?`.
BEZEL_TEST(type_param_identity_6315, override_param_widened_nested_other_parameter_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
end VB;
public class E<U, V>: VB<Array<U>>
  public constructor() end constructor;
  public override method M(x: Array<V>?): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A base parameter `(T) -> int` at `T = Array<U>` is `(Array<U>) -> int`, not `(Array<V>) -> int`.
BEZEL_TEST(type_param_identity_6315, override_param_function_typed_base_parameter_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(f: (T) -> int): int => 1;
end VB;
public class E<U, V>: VB<Array<U>>
  public constructor() end constructor;
  public override method M(f: (Array<V>) -> int): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `VB<Box<U>>`: a declared generic class nested around the other parameter, `Box<V>`.
BEZEL_TEST(type_param_identity_6315, override_param_user_generic_class_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class Box<Z>
  public constructor() end constructor;
end Box;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
end VB;
public class E<U, V>: VB<Box<U>>
  public constructor() end constructor;
  public override method M(x: Box<V>): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The abstract-base twin: `AB<Array<U>>`'s `M(x: T)` is not overridden by `M(x: Array<V>)`.
BEZEL_TEST(type_param_identity_6315, override_abstract_method_param_nested_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public abstract class AB<T>
  public constructor() end constructor;
  public abstract method M(x: T): int;
end AB;
public class E<U, V>: AB<Array<U>>
  public constructor() end constructor;
  public override method M(x: Array<V>): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The virtual-indexer twin of #6315: `VB<Array<U>>`'s key is `Array<U>`, not `Array<V>`.
BEZEL_TEST(type_param_identity_6315, override_indexer_key_nested_other_parameter_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual property [i: T]: int => 1;
end VB;
public class E<U, V>: VB<Array<U>>
  public constructor() end constructor;
  public override property [i: Array<V>]: int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `indexer` does not match the base member's index parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `Array<Array<V>>` is not `Array<Array<U>>` as a virtual indexer key.
BEZEL_TEST(type_param_identity_6315, override_indexer_key_doubly_nested_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual property [i: T]: int => 1;
end VB;
public class E<U, V>: VB<Array<Array<U>>>
  public constructor() end constructor;
  public override property [i: Array<Array<V>>]: int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `indexer` does not match the base member's index parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Return types match exactly (§12.1): `VB<U>`'s `R(): T` is `U`, not `V`.
BEZEL_TEST(type_param_identity_6315, override_return_bare_other_parameter_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method R(): T => throw new InvalidOperationException("n");
end VB;
public class E<U, V>: VB<U>
  public constructor() end constructor;
  public override method R(): V => throw new InvalidOperationException("n");
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `R` does not match the base member's return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `Array<V>` is not the substituted `Array<U>` return type.
BEZEL_TEST(type_param_identity_6315, override_return_nested_other_parameter_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method R(): T => throw new InvalidOperationException("n");
end VB;
public class E<U, V>: VB<Array<U>>
  public constructor() end constructor;
  public override method R(): Array<V> => throw new InvalidOperationException("n");
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `R` does not match the base member's return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A method's own type parameters correspond by position (§12.1, §11.2): `Q<X, Y>(x: X): X` does not override `Q<A, B2>(x: A): B2`.
BEZEL_TEST(type_param_identity_6315, override_return_own_type_parameter_wrong_position_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method Q<A, B2>(x: A): B2 => throw new InvalidOperationException("n");
end VB;
public class E<U>: VB<U>
  public constructor() end constructor;
  public override method Q<X, Y>(x: X): X => throw new InvalidOperationException("n");
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `Q` does not match the base member's return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `Q<A>(x: A): T` at `T = U` returns `U`; the override's own `B` is not it.
BEZEL_TEST(type_param_identity_6315, override_return_own_type_parameter_for_class_parameter_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method Q<A>(x: A): T => throw new InvalidOperationException("n");
end VB;
public class E<U>: VB<U>
  public constructor() end constructor;
  public override method Q<B>(x: B): B => throw new InvalidOperationException("n");
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `Q` does not match the base member's return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Two built-in exception classes have no declaration either: `ArgumentException?` is not `Exception?` as a return type.
BEZEL_TEST(type_param_identity_6315, override_return_builtin_exception_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method S(): Exception? => null;
end VB;
public class E<U>: VB<U>
  public constructor() end constructor;
  public override method S(): ArgumentException? => null;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `S` does not match the base member's return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `E<Z, W>: D<Z>`, `D<X>: VB<Array<X>>`: the base's `T` is `Array<Z>` in `E`'s scope, not `Array<W>`.
BEZEL_TEST(type_param_identity_6315, override_param_three_level_chain_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
  public virtual method R(): T => throw new InvalidOperationException("n");
  public virtual property [i: T]: int => 1;
end VB;
public class D<X>: VB<Array<X>>
  public constructor() end constructor;
end D;
public class E<Z, W>: D<Z>
  public constructor() end constructor;
  public override method M(x: Array<W>): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The return-type twin of the chain row.
BEZEL_TEST(type_param_identity_6315, override_return_three_level_chain_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
  public virtual method R(): T => throw new InvalidOperationException("n");
  public virtual property [i: T]: int => 1;
end VB;
public class D<X>: VB<Array<X>>
  public constructor() end constructor;
end D;
public class E<Z, W>: D<Z>
  public constructor() end constructor;
  public override method R(): Array<W> => throw new InvalidOperationException("n");
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `R` does not match the base member's return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The indexer-key twin of the chain row.
BEZEL_TEST(type_param_identity_6315, override_indexer_three_level_chain_rejected_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
  public virtual method R(): T => throw new InvalidOperationException("n");
  public virtual property [i: T]: int => 1;
end VB;
public class D<X>: VB<Array<X>>
  public constructor() end constructor;
end D;
public class E<Z, W>: D<Z>
  public constructor() end constructor;
  public override property [i: Array<W>]: int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `indexer` does not match the base member's index parameter types"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control: the override at the SAME parameter, nested, matches and dispatches through the base type.
BEZEL_TEST(type_param_identity_6315, override_same_parameter_nested_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): string => "base";
  public virtual method R(): T? => null;
  public virtual property P: string => "bp";
end VB;
public class E<U, V>: VB<Array<U>>
  public constructor() end constructor;
  public override method M(x: Array<U>): string => "derived";
  public override method R(): Array<U>? => null;
  public override property P: string => "dp";
end E;
public class Program
  public static method Main(): int
    var e := new E<int, string>();
    var a: Array<int> := [1, 2];
    Console.WriteLine(e.M(a));
    var b: VB<Array<int>> := e;
    Console.WriteLine(b.M(a));
    Console.WriteLine(b.P);
    if b.R() = null then Console.WriteLine("null"); end if;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "derived\nderived\ndp\nnull\n");
}

// Control: the §12.1 widening `Array<U>?` for the base's `T` at `Array<U>`.
BEZEL_TEST(type_param_identity_6315, override_widened_same_parameter_nested_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): string => "base";
end VB;
public class E<U, V>: VB<Array<U>>
  public constructor() end constructor;
  public override method M(x: Array<U>?): string => "derived";
end E;
public class Program
  public static method Main(): int
    var e := new E<int, string>();
    var a: Array<int> := [1];
    Console.WriteLine(e.M(a));
    var b: VB<Array<int>> := e;
    Console.WriteLine(b.M(a));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "derived\nderived\n");
}

// Control: a concrete instantiation `VB<Array<int>>` overridden at `Array<int>`.
BEZEL_TEST(type_param_identity_6315, override_substituted_concrete_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): string => "base";
  public virtual method R(): T? => null;
end VB;
public class E: VB<Array<int>>
  public constructor() end constructor;
  public override method M(x: Array<int>): string => "derived";
  public override method R(): Array<int>? => null;
end E;
public class Program
  public static method Main(): int
    var b: VB<Array<int>> := new E();
    var a: Array<int> := [1];
    Console.WriteLine(b.M(a));
    if b.R() = null then Console.WriteLine("null"); end if;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "derived\nnull\n");
}

// Control (§12.1 `Echo<U>` overrides `Echo<T>`): a method's own parameters renamed, in a parameter, a nested parameter and the return type.
BEZEL_TEST(type_param_identity_6315, override_own_type_parameters_renamed_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method Q<A>(x: A, y: T): string => "base";
  public virtual method G<A, C>(x: Array<A>, y: T): C? => null;
end VB;
public class E<U>: VB<U>
  public constructor() end constructor;
  public override method Q<B>(x: B, y: U): string => "derived";
  public override method G<P, Q2>(x: Array<P>, y: U): Q2? => null;
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    Console.WriteLine(e.Q("s", 1));
    var b: VB<int> := e;
    Console.WriteLine(b.Q<string>("s", 1));
    var a: Array<string> := ["s"];
    if b.G<string, string>(a, 1) = null then Console.WriteLine("null"); end if;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "derived\nderived\nnull\n");
}

// Control: through `D<X>: VB<Array<X>>`, `E<Z, W>: D<Z>` overrides at `Array<Z>` (method, and again over an intermediate override).
BEZEL_TEST(type_param_identity_6315, override_three_level_chain_same_parameter_accepted_spec_12_1) {
    auto r = runAot(R"(
module M;
public class VB<T>
  public constructor() end constructor;
  public virtual method M(x: T): string => "bm";
  public virtual method R(): T? => null;
end VB;
public class D<X>: VB<Array<X>>
  public constructor() end constructor;
  public override method M(x: Array<X>): string => "dm";
end D;
public class E<Z, W>: D<Z>
  public constructor() end constructor;
  public override method M(x: Array<Z>): string => "em";
  public override method R(): Array<Z>? => null;
end E;
public class Program
  public static method Main(): int
    var e := new E<int, string>();
    var a: Array<int> := [1];
    var d: D<int> := e;
    var b: VB<Array<int>> := e;
    Console.WriteLine(e.M(a));
    Console.WriteLine(d.M(a));
    Console.WriteLine(b.M(a));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "em\nem\nem\n");
}

// #6321 (§27.8, §12.1, v0.338): an alias is a pure synonym, so a generic alias
// of a generic alias (`type Id2<T> := Id<T>`) at `X` is `X`. Before the fix,
// resolving `Id2<X>` left the inner alias's parameter unsubstituted: the node
// was spelled `Id<T>` and carried the stale head `T`. The #6315 return-type
// spelling walk then reported a valid override as a mismatch. `Id2<int>` was
// also not `int`. The rows below are the issue's repros, then the parameter and
// index-key sites, then the #6315 rejects read through the alias.

// The issue repro: the base returns `Id2<X>`, the override returns `Q`.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_return_bare_parameter_accepted_6321_spec_27_8) {
    auto r = runAot(R"(
module A;
type Id<T> := T;
type Id2<T> := Id<T>;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public abstract method R(): Id2<X>;
end B;
public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method R(): Q => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    var e := new E<string>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// The issue repro: the base returns `Id2<Id<X>>`, the override returns `Q`.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_nested_return_bare_parameter_accepted_6321_spec_27_8) {
    auto r = runAot(R"(
module A;
type Id<T> := T;
type Id2<T> := Id<T>;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public abstract method R(): Id2<Id<X>>;
end B;
public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method R(): Q => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    var e := new E<string>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// Control: the alias on both sides (accepted before the fix as well).
BEZEL_TEST(type_param_identity_6315, alias_of_alias_return_both_sides_accepted_6321_spec_27_8) {
    auto r = runAot(R"(
module A;
type Id<T> := T;
type Id2<T> := Id<T>;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public abstract method R(): Id2<X>;
end B;
public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method R(): Id2<Q> => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    var e := new E<string>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// Control: the nested alias on both sides.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_nested_return_both_sides_accepted_6321_spec_27_8) {
    auto r = runAot(R"(
module A;
type Id<T> := T;
type Id2<T> := Id<T>;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public abstract method R(): Id2<Id<X>>;
end B;
public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method R(): Id2<Id<Q>> => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    var e := new E<string>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// A base-receiver call reaches the override, and the `Id2<X>` return carries the string. Before the fix this was rejected, and the same program accepted at the base checkout printed the string pointer as an integer.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_return_value_through_base_receiver_6321_spec_27_8) {
    auto r = runAot(R"(
module M;
type Id<A> := A;
type Id2<T> := Id<T>;
type Id3<U> := Id2<U>;
public class B<X>
  public constructor() end constructor;
  public virtual method R(x: X): Id2<X> => x;
  public virtual method R3(x: X): Id3<X> => x;
end B;
public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method R(x: Q): Q
    Console.WriteLine("derived");
    return x;
  end R;
  public override method R3(x: Q): Q
    Console.WriteLine("derived3");
    return x;
  end R3;
end E;
public class Program
  public static method Main(): int
    var b: B<string> := new E<string>();
    Console.WriteLine(b.R("a"));
    Console.WriteLine(b.R3("c"));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "derived\na\nderived3\nc\n");
}

// The parameter and index-key sites: `Id2<X>` and `Id2<Id<X>>` are `X`, so `Q` overrides them (rejected before the fix). The calls go through both receivers; #6323: the base receiver reaches the override (the vtable slot key reads the alias as its parameter).
BEZEL_TEST(type_param_identity_6315, alias_of_alias_parameter_and_index_key_accepted_6321_spec_27_8) {
    auto r = runAot(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id2<X>): string => "bm";
  public virtual method N(x: Id2<Id<X>>): string => "bn";
  public virtual property [i: Id2<X>]: string => "bi";
end B;
public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method M(x: Q): string => "dm";
  public override method N(x: Q): string => "dn";
  public override property [i: Q]: string => "di";
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    Console.WriteLine(e.M(1));
    Console.WriteLine(e.N(2));
    Console.WriteLine(e[3]);
    var b: B<int> := e;
    Console.WriteLine(b.M(1));
    Console.WriteLine(b.N(2));
    Console.WriteLine(b[3]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "dm\ndn\ndi\ndm\ndn\ndi\n");
}

// `Id2<int>` is `int` and `Id2<string>` is `string` at a field, a parameter and a local. Before the fix both were the unbound `T`, and `+` on them was rejected.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_value_type_is_the_aliased_type_6321_spec_27_8) {
    auto r = runAot(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public class C
  public constructor() end constructor;
  public F: Id2<int> := 0;
  public method Add(x: Id2<int>, y: Id2<string>): string => y + (x + 1).ToString();
end C;
public class Program
  public static method Main(): int
    var c := new C();
    c.F := 41;
    var v: Id2<long> := 5;
    Console.WriteLine(c.Add(c.F, "v="));
    Console.WriteLine(v + 1);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "v=42\n6\n");
}

// #6315 through the alias: `Id2<X>` is `X`, i.e. `Q`, not the class's other parameter `W`.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_return_other_parameter_rejected_6321_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public class B<X>
  public constructor() end constructor;
  public virtual method R(): Id2<X> => throw new Exception("x");
end B;
public class E<Q, W>: B<Q>
  public constructor() end constructor;
  public override method R(): W => throw new Exception("y");
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `R` does not match the base member's return type (§12.1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `Array<Id2<Id<X>>>` is `Array<Q>`, not `Array<W>`.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_nested_return_other_parameter_rejected_6321_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public class B<X>
  public constructor() end constructor;
  public virtual method R(): Array<Id2<Id<X>>> => throw new Exception("x");
end B;
public class E<Q, W>: B<Q>
  public constructor() end constructor;
  public override method R(): Array<W> => throw new Exception("y");
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `R` does not match the base member's return type (§12.1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The alias on the derived side: `Id2<W>` is `W`, not `X` bound to `Q`.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_derived_side_return_other_parameter_rejected_6321_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public class B<X>
  public constructor() end constructor;
  public virtual method R(): X => throw new Exception("x");
end B;
public class E<Q, W>: B<Q>
  public constructor() end constructor;
  public override method R(): Id2<W> => throw new Exception("y");
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `R` does not match the base member's return type (§12.1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The parameter site: `Id2<X>` is not `W`.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_param_other_parameter_rejected_6321_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id2<X>): int => 1;
end B;
public class E<Q, W>: B<Q>
  public constructor() end constructor;
  public override method M(x: W): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types (§12.1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `(Id2<X>) -> int` is `(Q) -> int`, not `(W) -> int`.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_function_param_other_parameter_rejected_6321_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public class B<X>
  public constructor() end constructor;
  public virtual method M(f: (Id2<X>) -> int): int => 1;
end B;
public class E<Q, W>: B<Q>
  public constructor() end constructor;
  public override method M(f: (W) -> int): int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `M` does not match the base member's parameter types (§12.1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The index-key site: `Id2<Id<X>>` is not `W`.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_index_key_other_parameter_rejected_6321_spec_12_1) {
    auto r = runAot(std::string(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public class B<X>
  public constructor() end constructor;
  public virtual property [i: Id2<Id<X>>]: int => 1;
end B;
public class E<Q, W>: B<Q>
  public constructor() end constructor;
  public override property [i: W]: int => 2;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "override of `indexer` does not match the base member's index parameter types (§12.1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// #6323 (§12.1, §27.8, v0.338): a base member whose parameter or index key is an
// alias of the class type parameter keyed its vtable slot by the alias's
// spelling (`X`), not by the substituted argument, so the override got a slot of
// its own and a base-receiver call ran the base body.

// The issue repro: `M(x: Id<X>)` overridden by `M(x: Q)`; the index key as well.
BEZEL_TEST(type_param_identity_6315, alias_parameter_base_receiver_dispatches_to_override_6323_spec_12_1) {
    auto r = runAot(R"(
module M;
type Id<T> := T;
public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id<X>): string => "base";
  public virtual property [i: Id<X>]: string => "bi";
end B;
public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method M(x: Q): string => "derived";
  public override property [i: Q]: string => "di";
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    var b: B<int> := e;
    Console.WriteLine(b.M(1));
    Console.WriteLine(e.M(1));
    Console.WriteLine(b[2]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "derived\nderived\ndi\n");
}

// The alias on both sides (`M(x: Id2<X>)` by `M(x: Id2<Q>)`, likewise `Id3`): dispatched correctly before #6321 only because both sides kept the same stale `T`; #6321 alone made it run the base body.
BEZEL_TEST(type_param_identity_6315, alias_of_alias_parameter_both_sides_base_receiver_6323_spec_12_1) {
    auto r = runAot(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
type Id3<U> := Id2<U>;
public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id2<X>): string => "bm";
  public virtual method N(x: Id3<X>): string => "bn";
  public virtual method P(x: Id2<Id<X>>): string => "bp";
end B;
public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method M(x: Id2<Q>): string => "dm";
  public override method N(x: Id3<Q>): string => "dn";
  public override method P(x: Id2<Id<Q>>): string => "dp";
end E;
public class Program
  public static method Main(): int
    var b: B<int> := new E<int>();
    Console.WriteLine(b.M(5));
    Console.WriteLine(b.N(5));
    Console.WriteLine(b.P(5));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "dm\ndn\ndp\n");
}

// An interface indexer keyed by an alias of its parameter (and a method over one), called through the interface. The single-alias indexer crashed (SIGSEGV) before the fix.
BEZEL_TEST(type_param_identity_6315, alias_interface_indexer_and_method_through_interface_6323_spec_11_2) {
    auto r = runAot(R"(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public interface I<X>
  readonly property [i: Id<X>]: string;
  method M(x: Id2<X>): string;
end I;
public interface J<X>
  readonly property [i: Id2<Id<X>>]: string;
end J;
public class E<Q>: I<Q>, J<Q>
  public constructor() end constructor;
  public property [i: Q]: string => "di";
  public method M(x: Q): string => "dm";
end E;
public class Program
  public static method Main(): int
    var e := new E<int>();
    var i: I<int> := e;
    var j: J<int> := e;
    Console.WriteLine(i[5]);
    Console.WriteLine(i.M(5));
    Console.WriteLine(j[5]);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "di\ndm\ndi\n");
}
