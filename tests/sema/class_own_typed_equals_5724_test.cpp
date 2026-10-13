// #5724 (§8.3, v0.338; proposal 0155): on a CLASS, a non-`override` `Equals`
// whose one parameter is the class's own type (`C`, `C?`, or `C<T>` with its
// own type parameters) fills the `Object` slot and obliges the class's own
// `override GetHashCode()`. An `Equals` at any other parameter type obliges
// nothing. Only the class's own `GetHashCode` override discharges it.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasOwnTypedHashDiag(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("must override `GetHashCode` (§8.3)") !=
                std::string::npos &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool hasAnyHashDiag(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("GetHashCode") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §8.3 (v0.338, #5724): the issue's repro. A class declaring `Equals(Point)`
// with no `GetHashCode` override is rejected at the `Equals`.
BEZEL_TEST(class_own_typed_equals_5724,
           own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module Repro;
class Point: IEquatable<Point>
  public readonly property X: int;
  public constructor(x: int) X := x; end constructor;
  public method Equals(other: Point): bool => X = other.X;
end Point;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasOwnTypedHashDiag(r, "declaring `Equals(Point)`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338): `C?` is the own type too, through the nullable spelling.
BEZEL_TEST(class_own_typed_equals_5724,
           nullable_own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
class C
  public constructor() end constructor;
  public method Equals(other: C?): bool => true;
end C;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasOwnTypedHashDiag(r, "declaring `Equals(C)`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338): a generic class's own type is `Box<T>` with its own type
// parameter; without a `GetHashCode` override it is rejected.
BEZEL_TEST(class_own_typed_equals_5724,
           generic_own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
class Box<T>
  public constructor() end constructor;
  public method Equals(other: Box<T>): bool => true;
end Box;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasOwnTypedHashDiag(r, "declaring `Equals(Box)`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338): an inherited `GetHashCode` override does NOT discharge the
// derived class's own-typed `Equals` obligation.
BEZEL_TEST(class_own_typed_equals_5724,
           inherited_gethashcode_does_not_discharge_spec_8_3) {
    auto r = runAot(R"(
module M;
class B
  public constructor() end constructor;
  public override method GetHashCode(): int => 0;
end B;
class D: B
  public constructor() base(); end constructor;
  public method Equals(other: D): bool => true;
end D;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasOwnTypedHashDiag(r, "declaring `Equals(D)`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338) control: an `Equals` taking `int` fills no slot and obliges
// nothing, with no `GetHashCode` declared.
BEZEL_TEST(class_own_typed_equals_5724,
           equals_at_other_type_obliges_nothing_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
class C: IEquatable<int>
  public constructor() end constructor;
  public method Equals(other: int): bool => true;
end C;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §8.3 (v0.338) control: a generic class's `Equals(other: Box<int>)` is a
// different instantiation, not its own type, so it obliges nothing.
BEZEL_TEST(class_own_typed_equals_5724,
           generic_other_instantiation_equals_obliges_nothing_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
class Box<T>
  public constructor() end constructor;
  public method Equals(other: Box<int>): bool => true;
end Box;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §8.3 (v0.338) control: an own-typed `Equals` with its own `override
// GetHashCode` is accepted.
BEZEL_TEST(class_own_typed_equals_5724,
           own_typed_equals_with_own_gethashcode_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
class C
  public constructor() end constructor;
  public method Equals(other: C): bool => true;
  public override method GetHashCode(): int => 0;
end C;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §8.3 (v0.338) control: a two-parameter `Equals(a, b)` comparer is not the
// one-parameter own-typed slot spelling, so it obliges nothing.
BEZEL_TEST(class_own_typed_equals_5724,
           two_parameter_equals_comparer_obliges_nothing_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
class C
  public constructor() end constructor;
  public method Equals(a: C, b: C): bool => true;
end C;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}
