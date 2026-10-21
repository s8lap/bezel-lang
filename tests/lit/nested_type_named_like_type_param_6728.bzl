// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #6728 / spec §14.1 (Type-parameter names), v0.340. "For the same reason
// a type may not declare a nested type (§11.6) named like one of its own type
// parameters, whatever the nested type's arity — the parameter has no qualified
// spelling ... — a compile error at the nested type's declaration naming this
// rule". The diagnostic is at the nested declaration (the column of its `class`
// keyword, where the declaration's span begins), one per offending nested type.
//
// Base accepted all three shapes below, and the nested type replaced the
// parameter in the body. The accept side (a nested type with another name, a
// nested type's own parameter reusing the spelling, a method type parameter or
// a property named like a nested type or a parameter) is pinned with its values
// in tests/sema/nested_type_type_param_name_6728_test.cpp.
module M;

public class Outer<Item>
  public class Item
  end Item;
  public method Get(): Item? => null;
end Outer;

public class Pair<A, B>
  public class B
  end B;
end Pair;

public class Holder<Item>
  public class Item<X>
  end Item;
end Holder;

// CHECK: :21:10: nested type `Item` takes the name of its enclosing type's type parameter `Item`; a type may not declare a nested type named like one of its own type parameters, whatever the nested type's arity
// CHECK-SAME: (§14.1, v0.340)
// CHECK: :27:10: nested type `B` takes the name of its enclosing type's type parameter `B`; a type may not declare a nested type named like one of its own type parameters, whatever the nested type's arity
// CHECK-SAME: (§14.1, v0.340)
// CHECK: :32:10: nested type `Item` takes the name of its enclosing type's type parameter `Item`; a type may not declare a nested type named like one of its own type parameters, whatever the nested type's arity
// CHECK-SAME: (§14.1, v0.340)
