// Regression tests for §14.1 type-parameter scope rules:
// (1) type-parameter list distinctness: duplicate names rejected
// (2) method type parameter shadowing enclosing type parameter rejected
// (3) bare generic type reference rejected at type reference sites
//
// Fix §14.1 bare generic rejection in TypeResolution_Names.cpp:
// `!t.genericArgs.empty()` guard skipped the arity check for bare
// (zero-arity) references to generic types — those must also be rejected.
// The builtin-fallback block below already handles the case where a bare
// name matches an always-available builtin at arity 0.
//
// Spec (v0.334, §14.1):
// "The names in one type-parameter list are distinct, and a generic method's
//  type parameter may not reuse the name of a type parameter in scope at
//  the method's declaration, including its enclosing type's."
// "Every such reference supplies exactly the declared number of type arguments.
//  Bezel has no raw and no open generic type reference: `Box` bare and
//  `Box<int, string>` are equally compile errors at a `class Box<T>`."

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Diagnostics for §14.1 type-parameter scope rules.
constexpr const char* kBareGenericDiag = "generic-argument count does not match type's generic-parameter count";
constexpr const char* kDuplicateParamDiag = "duplicate type parameter name";
constexpr const char* kShadowParamDiag = "reuses the name of a type parameter of enclosing type";

bool rejectedWithDiag(const std::string& program, const char* diag) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(diag) != std::string::npos)
            return true;
    }
    return false;
}

bool accepted(const std::string& program) {
    auto r = runAot(program);
    return r.ran;
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════════════
// §14.1: type-parameter list distinctness
// "The names in one type-parameter list are distinct"
// ══════════════════════════════════════════════════════════════════════════════

BEZEL_TEST(sema_generic_type_param_scope_5880,
           duplicate_type_param_name_in_class_rejected_spec_14_1) {
    // class Box<T, T> — duplicate name in class type-parameter list
    bool rejected = rejectedWithDiag(R"BZ(
module Conf;
public class Box<T, T>
end Box;
public class Program
  public static method Main()
  end Main;
end Program;
)BZ", kDuplicateParamDiag);
    BEZEL_EXPECT_TRUE(rejected);
}

BEZEL_TEST(sema_generic_type_param_scope_5880,
           duplicate_type_param_name_in_method_rejected_spec_14_1) {
    // method Foo<U, U> — duplicate name in method type-parameter list
    bool rejected = rejectedWithDiag(R"BZ(
module Conf;
public class Container
  public method Foo<U, U>(x: U)
  end Foo;
  public constructor()
  end constructor;
end Container;
public class Program
  public static method Main()
  end Main;
end Program;
)BZ", kDuplicateParamDiag);
    BEZEL_EXPECT_TRUE(rejected);
}

// ══════════════════════════════════════════════════════════════════════════════
// §14.1: method type parameter may not reuse enclosing type's name
// "a generic method's type parameter may not reuse the name of a type parameter
//  in scope at the method's declaration, including its enclosing type's"
// ══════════════════════════════════════════════════════════════════════════════

BEZEL_TEST(sema_generic_type_param_scope_5880,
           method_type_param_shadowing_enclosing_type_param_rejected_spec_14_1) {
    // class Container<T> { method Foo<T>(x: T) } — method T shadows class T
    bool rejected = rejectedWithDiag(R"BZ(
module Conf;
public class Container<T>
  public method Foo<T>(x: T)
  end Foo;
  public constructor()
  end constructor;
end Container;
public class Program
  public static method Main()
  end Main;
end Program;
)BZ", kShadowParamDiag);
    BEZEL_EXPECT_TRUE(rejected);
}

BEZEL_TEST(sema_generic_type_param_scope_5880,
           method_type_param_bound_to_enclosing_type_param_accepted_spec_14_1) {
    // class Container<T> { method Store<U>(item: U) where U: T } — ok, different name
    bool ok = accepted(R"BZ(
module Conf;
public class Container<T>
  public method Store<U>(item: U) where U: T
    Console.WriteLine("stored");
  end Store;
  public constructor()
  end constructor;
end Container;
public class Program
  public static method Main()
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(ok);
}

// ══════════════════════════════════════════════════════════════════════════════
// §14.1: bare generic type reference rejected
// "Every such reference supplies exactly the declared number of type arguments.
//  Bezel has no raw and no open generic type reference: `Box` bare is a compile
//  error at a `class Box<T>`."
// ══════════════════════════════════════════════════════════════════════════════

// Bare reference to generic class in variable declaration.
BEZEL_TEST(sema_generic_type_param_scope_5880,
           bare_generic_type_ref_in_var_decl_rejected_spec_14_1) {
    bool rejected = rejectedWithDiag(R"BZ(
module Conf;
public class Box<T>
  public constructor() end constructor;
end Box;
public class Program
  public static method Main()
    var b: Box;
  end Main;
end Program;
)BZ", kBareGenericDiag);
    BEZEL_EXPECT_TRUE(rejected);
}

// Bare reference to generic class in field declaration.
BEZEL_TEST(sema_generic_type_param_scope_5880,
           bare_generic_type_ref_in_field_decl_rejected_spec_14_1) {
    bool rejected = rejectedWithDiag(R"BZ(
module Conf;
public class Box<T>
  public constructor() end constructor;
end Box;
public class Container
  private _box: Box;
  public constructor() end constructor;
end Container;
public class Program
  public static method Main()
  end Main;
end Program;
)BZ", kBareGenericDiag);
    BEZEL_EXPECT_TRUE(rejected);
}

// Bare reference to generic class in parameter declaration.
BEZEL_TEST(sema_generic_type_param_scope_5880,
           bare_generic_type_ref_in_param_decl_rejected_spec_14_1) {
    bool rejected = rejectedWithDiag(R"BZ(
module Conf;
public class Box<T>
  public constructor() end constructor;
end Box;
public class Container
  public method Take(b: Box)
  end Take;
  public constructor()
  end constructor;
end Container;
public class Program
  public static method Main()
  end Main;
end Program;
)BZ", kBareGenericDiag);
    BEZEL_EXPECT_TRUE(rejected);
}

// Differential control: explicit type arguments still accepted.
BEZEL_TEST(sema_generic_type_param_scope_5880,
           explicit_type_args_still_accepted_spec_14_1) {
    bool ok = accepted(R"BZ(
module Conf;
public class Box<T>
  public constructor() end constructor;
end Box;
public class Program
  public static method Main()
    var b: Box<string> := new Box<string>();
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(ok);
}

// Differential control: bare reference to non-generic class still accepted.
BEZEL_TEST(sema_generic_type_param_scope_5880,
           bare_ref_to_non_generic_class_still_accepted_spec_14_1) {
    bool ok = accepted(R"BZ(
module Conf;
public class Box
  public constructor() end constructor;
end Box;
public class Program
  public static method Main()
    var b: Box := new Box();
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(ok);
}

// User generic shadowing builtin: bare reference to user List<K> must still reject
// even though List (builtin, arity 1) is always-available.
// This exercises the #5172 builtin-fallback path: builtinAr=1 != t.genericArgs.size()=0,
// so it falls through to the arity error — NOT the misleading builtin diagnostic.
BEZEL_TEST(sema_generic_type_param_scope_5880,
           user_generic_shadowing_builtin_bare_ref_rejected_spec_14_1) {
    bool rejected = rejectedWithDiag(R"BZ(
module Conf;
public class List<T>
  public constructor() end constructor;
end List;
public class Program
  public static method Main()
    var l: List;
  end Main;
end Program;
)BZ", kBareGenericDiag);
    BEZEL_EXPECT_TRUE(rejected);
}

// User generic shadowing builtin: explicit type args still accepted.
BEZEL_TEST(sema_generic_type_param_scope_5880,
           user_generic_shadowing_builtin_explicit_args_accepted_spec_14_1) {
    bool ok = accepted(R"BZ(
module Conf;
public class List<T>
  public constructor() end constructor;
end List;
public class Program
  public static method Main()
    var l: List<int> := new List<int>();
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(ok);
}
