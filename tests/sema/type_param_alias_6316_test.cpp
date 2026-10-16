// #6316 (§27.8, §12.1, §11.2, v0.338): a type alias is "a pure synonym" and identity sees through it. typeRefEqualSameScope compares class type parameters by spelling, but an alias expansion (`type Id<T> := T`) keeps the alias declaration's head, not the use-site parameter, so spelling is not evidence there and the compare falls back to the blind one. Each program is VALID and must compile and run.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;

BEZEL_TEST(type_param_alias_6316, alias_to_bare_param_property_override_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public abstract property V: Id<X>;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override property V: P => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_to_nullable_param_property_override_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public abstract property V: Nl<X>;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override property V: Nl<P> => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_to_bare_param_indexer_value_override_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public abstract property [i: int]: Id<X>;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override property [i: int]: P => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_of_alias_property_override_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
type Arr<T> := Array<T>;
public abstract class B<X>
  public constructor() end constructor;
  public abstract property V: Id2<X>;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override property V: P => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_to_array_property_override_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
type Arr<T> := Array<T>;
public abstract class B<X>
  public constructor() end constructor;
  public abstract property V: Arr<X>?;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override property V: Arr<P>? => null;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_to_array_vs_spelled_array_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
type Arr<T> := Array<T>;
public abstract class B<X>
  public constructor() end constructor;
  public abstract property V: Arr<X>?;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override property V: Array<P>? => null;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_indexer_key_abstract_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public abstract property [i: Id<X>]: int;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override property [i: P]: int => 1;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_interface_property_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
public interface I<T>
  readonly property Val: Id<T>;
end I;
public class E<P>: I<P>
  public constructor() end constructor;
  public property Val: P => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_interface_indexer_key_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
public interface I<T>
  readonly property [i: Id<T>]: int;
end I;
public class E<P>: I<P>
  public constructor() end constructor;
  public property [i: P]: int => 1;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_method_param_override_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id<X>): int return 1; end M;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override method M(x: P): int return 2; end M;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_method_return_override_accepted_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
public abstract class B<X>
  public constructor() end constructor;
  public virtual method M(): Id<X> throw new Exception("x"); end M;
end B;
public class E<P>: B<P>
  public constructor() end constructor;
  public override method M(): P throw new Exception("x"); end M;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

BEZEL_TEST(type_param_alias_6316, alias_generic_iface_property_wrong_parameter_stays_rejected_spec_11_2) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public interface GI<X, Y>
  readonly property Val: Id<Y>;
end GI;
public class K<P, Q>: GI<P, Q>
  public constructor() end constructor;
  public property Val: Id<P> => throw new Exception("x");
end K;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(type_param_alias_6316, alias_generic_iface_indexer_key_wrong_parameter_stays_rejected_spec_11_2) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public interface GI<X, Y>
  readonly property [i: Id<Y>]: int;
end GI;
public class K<P, Q>: GI<P, Q>
  public constructor() end constructor;
  public property [i: Id<P>]: int => 1;
end K;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
