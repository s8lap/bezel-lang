// #5956 (§12.1 / §12.2 / §8.3, v0.338): an `override` of one of `Object`'s
// three virtual members at a parameter list other than its own matches no
// virtual member, and a wrong-signature `GetHashCode` does not discharge the
// §8.3 pairing obligation.
// #5961 (§12.2, v0.338): "a static member of the three names stays blocked" —
// a static `Equals`/`GetHashCode`/`ToString` is rejected at any parameter list.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool hasAnyObjectTrioDiag(const JitResult& r) {
    return hasErrorContaining(r, "matches no virtual member") ||
           hasErrorContaining(r, "hides the inherited `Object.") ||
           hasErrorContaining(r, "must also override `GetHashCode`") ||
           hasErrorContaining(r, "must override `GetHashCode`");
}

}  // namespace

// §12.1 / §12.2 (v0.338, #5956): the issue's repro. The `override GetHashCode(x:
// §8.3 (v0.338, #5956): a wrong-signature `GetHashCode` override does not
// §12.1 / §12.2 (v0.338, #5956): `override Equals` at a parameter list other
// §12.1 / §12.2 (v0.338, #5956): `override ToString` at `ToString(fmt: string)`
// §12.2 (v0.338, #5961): a static `GetHashCode` at another parameter list is
// blocked: "a static member of the three names stays blocked".
BEZEL_TEST(object_trio_override_static_5956,
           static_gethashcode_other_signature_spec_12_2) {
    auto r = runAot(R"(
module M;
public class Point
  public constructor() end constructor;
  public static method GetHashCode(x: int): int => x;
end Point;
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static member `GetHashCode` hides the inherited `Object.GetHashCode`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.338, #5961): a static `Equals` at `Equals(a: int, b: int)` is
// blocked at that parameter list too.
BEZEL_TEST(object_trio_override_static_5956,
           static_equals_other_signature_spec_12_2) {
    auto r = runAot(R"(
module M;
public class C
  public constructor() end constructor;
  public static method Equals(a: C, b: C): bool => true;
end C;
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static member `Equals` hides the inherited `Object.Equals`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.338, #5961): a static `ToString(fmt: string)` is blocked.
BEZEL_TEST(object_trio_override_static_5956,
           static_tostring_other_signature_spec_12_2) {
    auto r = runAot(R"(
module M;
public class Point
  public constructor() end constructor;
  public static method ToString(fmt: string): string => fmt;
end Point;
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static member `ToString` hides the inherited `Object.ToString`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 / §8.3 (v0.338) control: the correct own-signature overrides are
// accepted, with the pairing obligation discharged by `GetHashCode()`.
BEZEL_TEST(object_trio_override_static_5956,
           own_signature_overrides_accepted_spec_12_2) {
    auto r = runAot(R"(
module M;
public class Point
  public constructor() end constructor;
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): int => 0;
  public override method ToString(): string => "p";
end Point;
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyObjectTrioDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §8.3 (v0.338) control: the own-typed `Equals(other: Point)` is the slot's
// second spelling and is an accepted override target, discharged the same way.
BEZEL_TEST(object_trio_override_static_5956,
           own_typed_override_equals_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
public class Point
  public constructor() end constructor;
  public override method Equals(other: Point): bool => true;
  public override method GetHashCode(): int => 0;
end Point;
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyObjectTrioDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §12.2 (v0.338) control: plain (non-override) instance members at the other
// parameter lists are admitted as overloads.
BEZEL_TEST(object_trio_override_static_5956,
           plain_other_signatures_admitted_spec_12_2) {
    auto r = runAot(R"(
module M;
public class Point
  public constructor() end constructor;
  public method Equals(a: int, b: int): bool => true;
  public method GetHashCode(value: int): int => value;
  public method ToString(fmt: string): string => fmt;
end Point;
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyObjectTrioDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}
