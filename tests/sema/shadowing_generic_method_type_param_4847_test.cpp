// Regression tests for issue #4847 (§14.1/§14.5, v0.307).
//
// Bug: checkArgsAgainstParams passed the method's own generic parameter names
// (calleeMethodGenericParamNames) as the freeTypeParamNames set to
// checkAssignable's #4810 arm. When the method's T shadows the class's T,
// T was in calleeMethodGenericParamNames AND in currentScopeGenericParamNames.
// The #4810 arm then skipped the check via the currentScopeGenericParamNames()
// fallback, never verifying the formal.
//
// Fix: pass calleeMethodGenericParamNames separately to the #4810 arm so it
// can skip the currentScopeGenericParamNames() fallback for the CALLEE's own
// generic params. The #4810 arm now exempts a name only when it is NOT
// the callee's own generic param.
//
// Note: the shadowing cases (both explicit type arg and inference) pass sema
// with the fix but hit a codegen limitation (#4862): codegen's name-keyed
// substitution maps cannot bind both the class's T and the method's T when
// they share a name. Both cases are rejected (codegen IR verification failure).
// The non-shadowing controls pass correctly.

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

// ════════ Controls: non-shadowing should still work ════════════════════════

BEZEL_TEST(shadowing_generic_method_type_param_4847,
           no_shadowing_same_pattern_still_works_control_spec_14_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Box<T>
  public method Id<V>(x: V): V
    return x;
  end Id;
  public method Go(): int
    return this.Id(5) + 1;
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

BEZEL_TEST(shadowing_generic_method_type_param_4847,
           same_named_caller_param_int_literal_still_works_control_spec_14_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Util
  public static method Id<T>(x: T): T
    return x;
  end Id;
  public static method Go<T>(v: T): int
    return Id(5);
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<int>(1));
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// ════════ Shadowing cases: reject (codegen #4862 limitation) ════════════════

// The shadowing method T shadows the class T. With the sema fix, both explicit
// type arg and inference pass sema. But codegen cannot handle this shape because
// its name-keyed substitution maps cannot bind both T's. Both reject (no codegen).

// Explicit type arg: Box<T>.Id<int>(5) inside Box<T>.Go()
BEZEL_TEST(shadowing_generic_method_type_param_4847,
           explicit_type_arg_in_shadowing_method_rejected_codegen_4862_spec_14_5) {
    BEZEL_EXPECT_TRUE(!runAot(R"BZ(
module Conf;
public class Box<T>
  public method Id<T>(x: T): T
    return x;
  end Id;
  public method Go(): int
    return this.Id<int>(5) + 1;
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ").ran);
}

// Static method explicit type arg.
BEZEL_TEST(shadowing_generic_method_type_param_4847,
           static_explicit_type_arg_in_shadowing_method_rejected_codegen_4862_spec_14_5) {
    BEZEL_EXPECT_TRUE(!runAot(R"BZ(
module Conf;
public class Box<T>
  public static method Id<T>(x: T): T
    return x;
  end Id;
  public method Go(): int
    return Box<T>.Id<int>(5) + 1;
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ").ran);
}

// Inference case: Id(5) inside Box<T>.Id<T>
BEZEL_TEST(shadowing_generic_method_type_param_4847,
           inference_in_shadowing_method_rejected_codegen_4862_spec_14_5) {
    BEZEL_EXPECT_TRUE(!runAot(R"BZ(
module Conf;
public class Box<T>
  public method Id<T>(x: T): T
    return x;
  end Id;
  public method Go(): int
    return this.Id(5) + 1;
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ").ran);
}

// Static inference case.
BEZEL_TEST(shadowing_generic_method_type_param_4847,
           static_inference_in_shadowing_method_rejected_codegen_4862_spec_14_5) {
    BEZEL_EXPECT_TRUE(!runAot(R"BZ(
module Conf;
public class Box<T>
  public static method Id<T>(x: T): T
    return x;
  end Id;
  public method Go(): int
    return Box<T>.Id(5) + 1;
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ").ran);
}
