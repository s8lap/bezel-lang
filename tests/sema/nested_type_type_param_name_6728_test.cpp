// #6728 (§14.1 Type-parameter names, v0.340): "a type may not declare a nested
// type ([§11.6]) named like one of its own type parameters, whatever the nested
// type's arity — the parameter has no qualified spelling ... a compile error at
// the nested type's declaration naming this rule". Base accepted every reject
// row below (MEASURED on 97f4e965), and the nested type silently replaced the
// parameter in the body. The accept rows are the controls the same paragraph
// and §11.6 keep admitted: a nested type with another name, a nested type's own
// parameter reusing the spelling (§11.6), a method's type parameter or a
// property sharing a nested type's or a parameter's name ("a member of any
// other kind may share a type parameter's name"), and an inherited nested type
// that a derived class's own parameter outranks (§2.7, Through nesting).

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

// The rejection the §14.1 rule names. Pinned on the message text, not only on
// the presence of some error, so an unrelated diagnostic cannot satisfy it.
const char* kNestedNeedle =
    "a type may not declare a nested type named like one of its own type "
    "parameters, whatever the nested type's arity";

}  // namespace

// §14.1 (v0.340, #6728): the issue's repro. `Outer<Item>` declares a non-generic
// nested class `Item`, which would overwrite the parameter in the body.
BEZEL_TEST(nested_type_type_param_name_6728,
           nested_class_named_like_outer_type_param_rejected_spec_14_1) {
    auto r = runAot(R"(
module M;
public class Outer<Item>
  public class Item
  end Item;
  public method Get(): Item? => null;
end Outer;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `Item` takes the name of its enclosing type's type "
           "parameter `Item`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNestedNeedle));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(§14.1, v0.340)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.1 (v0.340, #6728): "whatever the nested type's arity". A GENERIC nested
// class `Item<X>` under `Outer<Item>` is still named like the parameter.
BEZEL_TEST(nested_type_type_param_name_6728,
           nested_generic_class_named_like_type_param_rejected_spec_14_1) {
    auto r = runAot(R"(
module M;
public class Outer<Item>
  public class Item<X>
  end Item;
  public method Get(): int => 0;
end Outer;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `Item` takes the name of its enclosing type's type "
           "parameter `Item`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.1 (v0.340, #6728): the rule covers EVERY parameter of the list, not only
// the first. `Pair<A, B>` with a nested class `B`.
BEZEL_TEST(nested_type_type_param_name_6728,
           nested_class_named_like_second_type_param_rejected_spec_14_1) {
    auto r = runAot(R"(
module M;
public class Pair<A, B>
  public class B
  end B;
end Pair;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `B` takes the name of its enclosing type's type "
           "parameter `B`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNestedNeedle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.1 (v0.340, #6728): the rule binds a RECORD's parameter list too: a
// `record Holder<Node>` declaring a nested class `Node`.
BEZEL_TEST(nested_type_type_param_name_6728,
           nested_class_in_record_named_like_type_param_rejected_spec_14_1) {
    auto r = runAot(R"(
module M;
public record Holder<Node>(Value: int)
  public class Node
  end Node;
end Holder;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `Node` takes the name of its enclosing type's type "
           "parameter `Node`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.1 (v0.340, #6728): the single-letter spelling of the same rule.
BEZEL_TEST(nested_type_type_param_name_6728,
           nested_class_named_T_under_generic_T_rejected_spec_14_1) {
    auto r = runAot(R"(
module M;
public class Outer<T>
  public class T
  end T;
end Outer;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `T` takes the name of its enclosing type's type "
           "parameter `T`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Accept control (§14.1, #6728): a nested type with a name other than the
// parameter's leaves both the nested type and the parameter usable.
BEZEL_TEST(nested_type_type_param_name_6728,
           nested_class_with_other_name_accepted_spec_14_1) {
    auto r = runAot(R"(
module M;
public class Outer<Item>
  public class Node
  end Node;
  public method Wrap(x: Item): Item => x;
end Outer;
public class Program
  public static method Main(): int
    var o := new Outer<int>();
    return o.Wrap(7);
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// Accept control (§11.6, #6728): a nested type's OWN type parameter may reuse
// the enclosing type's spelling, since the enclosing parameter is not in scope
// in a nested type. `Inner<T>` is the nested type, not a nested type named `T`.
// Compile-only: a `return Outer<int>.Inner<int>.Id(7)` spelling does not parse
// (the expression form is a separate parser question), so Main does not call it.
BEZEL_TEST(nested_type_type_param_name_6728,
           nested_generic_reusing_outer_param_spelling_accepted_spec_11_6) {
    auto r = runAot(R"(
module M;
public class Outer<T>
  public class Inner<T>
    public static method Id(x: T): T => x;
  end Inner;
end Outer;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ran\n");
}

// Accept control (§14.1, #6728): a class named `T` declared inside a NON-generic
// nested class of `Outer<T>` names no parameter of its own type. The rule is
// about the declaring type's own parameters, and `Inner` has none.
BEZEL_TEST(nested_type_type_param_name_6728,
           nested_class_named_T_inside_non_generic_nested_accepted_spec_11_6) {
    auto r = runAot(R"(
module M;
public class Outer<T>
  public class Inner
    public class T
    end T;
  end Inner;
end Outer;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ran\n");
}

// Accept control (§14.1, #6728): "a generic method's type parameter may not
// reuse the name of a type parameter in scope" is the only parameter rule. A
// method type parameter named like a NESTED TYPE of its class is admitted.
BEZEL_TEST(nested_type_type_param_name_6728,
           method_type_param_named_like_nested_type_accepted_spec_14_1) {
    auto r = runAot(R"(
module M;
public class Outer<Item>
  public class Node
  end Node;
  public method Pick<Node>(x: Node): Node => x;
end Outer;
public class Program
  public static method Main(): int
    var o := new Outer<int>();
    return o.Pick<int>(7);
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// Accept control (§14.1, #6728): "a member of any other kind may share a type
// parameter's name": `public property Value: Value` in `Cell<Value>`.
BEZEL_TEST(nested_type_type_param_name_6728,
           property_named_like_type_param_accepted_spec_14_1) {
    auto r = runAot(R"(
module M;
public class Cell<Value>
  public property Value: Value;
  public constructor(v: Value)
    this.Value := v;
  end constructor;
end Cell;
public class Program
  public static method Main(): int
    var c := new Cell<int>(7);
    return c.Value;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// Accept control (§2.7 Through nesting, §14.1, #6728): `D<Item>` inherits a
// nested `Item` from `Base`, and its own parameter is found before the
// inherited nested type ("at a type's own link its type parameter is found
// before a nested type of the name the type inherits"). Nothing is declared
// here, so the rule does not reach it.
BEZEL_TEST(nested_type_type_param_name_6728,
           inherited_nested_name_loses_to_own_param_accepted_spec_2_7) {
    auto r = runAot(R"(
module M;
public class Base
  public class Item
  end Item;
end Base;
public class D<Item>: Base
  public method Wrap(x: Item): Item => x;
end D;
public class Program
  public static method Main(): int
    var d := new D<int>();
    return d.Wrap(7);
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
