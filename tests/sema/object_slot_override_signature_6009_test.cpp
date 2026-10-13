// #6009 (§12.1 / §8.1, v0.338): an override of one of Object's virtual members must match its visibility (public), return type, parameter modifiers and type-parameter count exactly. §12.1: "Visibility, return type, parameter types, and parameter modifiers must match the base member exactly"; "Parameter modifiers that must match: `ref`, `out`, `owned`, `scoped`".
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

// #6009 repro 1: the `ref` form compiled and SEGVed at run time.
BEZEL_TEST(object_slot_override_signature_6009, ref_modifier_on_equals_override_spec_12_1_issue_repro) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(ref other: Object?): bool => other = null;
  public override method GetHashCode(): int => 7;

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.Equals`'s parameter modifiers"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `out` is a parameter modifier that must match (§12.1); the body assigns the out so no earlier check fires.
BEZEL_TEST(object_slot_override_signature_6009, out_modifier_on_equals_override_spec_12_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(out other: Object?): bool
    other := null;
    return true;
  end Equals;
  public override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.Equals`'s parameter modifiers"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// #6009 repro 2: return type must match exactly.
BEZEL_TEST(object_slot_override_signature_6009, int_return_on_equals_override_spec_12_1_issue_repro) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): int => 5;
  public override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.Equals`'s return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// #6009: was an IR-verification failure in codegen instead of a diagnostic.
BEZEL_TEST(object_slot_override_signature_6009, int_return_on_tostring_override_spec_12_1_issue_repro) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): int => 7;
  public override method ToString(): int => 7;

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.ToString`'s return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// return type must match exactly (§12.1: no covariance in this version).
BEZEL_TEST(object_slot_override_signature_6009, long_return_on_gethashcode_override_spec_12_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): long => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.GetHashCode`'s return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// no declared return type is not `int`.
BEZEL_TEST(object_slot_override_signature_6009, unit_return_on_gethashcode_override_spec_12_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode() => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.GetHashCode`'s return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// #6009 repro 3: Object's members are public (§8.1).
BEZEL_TEST(object_slot_override_signature_6009, private_tostring_override_spec_12_1_issue_repro) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): int => 7;
  private override method ToString(): string => "priv";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.ToString`'s visibility"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// visibility must match.
BEZEL_TEST(object_slot_override_signature_6009, protected_gethashcode_override_spec_12_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => true;
  protected override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.GetHashCode`'s visibility"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// visibility must match.
BEZEL_TEST(object_slot_override_signature_6009, internal_equals_override_spec_12_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  internal override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.Equals`'s visibility"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// no visibility modifier is private, as the user-base path already reads it.
BEZEL_TEST(object_slot_override_signature_6009, unmarked_visibility_tostring_override_spec_12_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): int => 7;
  override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.ToString`'s visibility"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.1: an override declares exactly as many type parameters as the overridden member (none).
BEZEL_TEST(object_slot_override_signature_6009, generic_tostring_override_spec_12_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): int => 7;
  public override method ToString<T>(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match `Object.ToString`'s type parameter count"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The spec-admitted control: all three own-signature public overrides dispatch through Object.
BEZEL_TEST(object_slot_override_signature_6009, own_signature_trio_override_dispatches_spec_8_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => false;
  public override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n7\ns\n");
}

// §8.3 / §12.1: Equals at the class's own type is the slot's second spelling and stays admitted.
BEZEL_TEST(object_slot_override_signature_6009, own_typed_equals_second_spelling_dispatches_spec_8_3) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: C?): bool => true;
  public override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n7\ns\n");
}

// §27.8: an alias is a pure synonym; `OAlias?` is `Object?`.
BEZEL_TEST(object_slot_override_signature_6009, alias_spelled_object_param_is_the_same_type_spec_27_8) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: OAlias?): bool => true;
  public override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n7\ns\n");
}

// §8.1: `System.Object` reaches Object.
BEZEL_TEST(object_slot_override_signature_6009, qualified_object_param_spec_8_1) {
    auto r = runAot(R"(
module M;
type OAlias := Object;
public class C
  public constructor() end constructor;
  public override method Equals(other: System.Object?): bool => true;
  public override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";

end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n7\ns\n");
}

// 3-level hierarchy: a Leaf override of Mid's overrides (user-base path) and Mid's of Object's, both with matching signatures.
BEZEL_TEST(object_slot_override_signature_6009, three_level_hierarchy_overrides_dispatch_spec_12_1) {
    auto r = runAot(R"(
module M;
public class Mid
  public constructor() end constructor;
  public override method ToString(): string => "mid";
  public override method GetHashCode(): int => 1;
  public override method Equals(other: Object?): bool => false;
end Mid;
public class Leaf: Mid
  public constructor() end constructor;
  public override method ToString(): string => "leaf";
  public override method GetHashCode(): int => 2;
end Leaf;
public class Program
  public static method Main(): int
    var l: Leaf := new Leaf();
    var o: Object := l;
    Console.WriteLine(o.ToString());
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.Equals(null));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "leaf\n2\nFalse\n");
}

// user-base path control (unchanged): a private override of Mid's public override.
BEZEL_TEST(object_slot_override_signature_6009, three_level_hierarchy_private_override_rejected_spec_12_1) {
    auto r = runAot(R"(
module M;
public class Mid
  public constructor() end constructor;
  public override method ToString(): string => "mid";
  public override method GetHashCode(): int => 1;
  public override method Equals(other: Object?): bool => false;
end Mid;
public class Leaf: Mid
  public constructor() end constructor;
  private override method ToString(): string => "leaf";
  public override method GetHashCode(): int => 2;
end Leaf;
public class Program
  public static method Main(): int
    var l: Leaf := new Leaf();
    var o: Object := l;
    Console.WriteLine(o.ToString());
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(o.Equals(null));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not match the base member's visibility"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
