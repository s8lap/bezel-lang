// #6725 (§8.3, v0.340, proposal 0196): the `Object`-slot `Equals` declares no
// default value for its parameter. §8.3: "The slot's parameter declares **no
// default value** ([§22.5]): on a class in either spelling and on a value kind
// alike, `Equals(other: C := …)` is a compile error at the declaration". The
// §9.2 / §10.5 / §35.2 value-kind rows restate it for each kind's own-typed
// `Equals`. Reject rows pin the diagnostic and a real location; control rows
// pin that the same member names with other parameter lists, or with a
// default on a non-slot parameter, still compile and run.
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

// ---- reject rows ------------------------------------------------------------

// §7.4 of the code-generation reference, verbatim shape: a record's own-typed
// `Equals` with a default parameter value.
BEZEL_TEST(object_slot_equals_default_6725, record_own_typed_default_spec_8_3_coin_7_4) {
    auto r = runAot(R"(
module M;
public record Coin(Amount: int)
  public method Equals(other: Coin := default(Coin)): bool => Amount = other.Amount;
  public method GetHashCode(): int => Amount;
end Coin;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of record `Coin` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A class's own-typed `Equals(other: C?)` with a default, `override`.
BEZEL_TEST(object_slot_equals_default_6725, class_own_nullable_default_override_spec_8_3) {
    auto r = runAot(R"(
module M;
public class C
  public override method Equals(other: C? := null): bool => true;
  public override method GetHashCode(): int => 0;
end C;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of class `C` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The other spelling of a class's slot: `override Equals(other: Object?)`.
BEZEL_TEST(object_slot_equals_default_6725, class_objectq_default_override_spec_8_3) {
    auto r = runAot(R"(
module M;
public class C
  public override method Equals(other: Object? := null): bool => true;
  public override method GetHashCode(): int => 0;
end C;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of class `C` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Plain (no `override`) own-typed spelling on a class, non-nullable parameter.
BEZEL_TEST(object_slot_equals_default_6725, class_plain_own_nonnull_default_spec_8_3) {
    auto r = runAot(R"(
module M;
public class C
  public method Equals(other: C := new C()): bool => true;
  public override method GetHashCode(): int => 0;
end C;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of class `C` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A nested class's slot is reached by the same walk as a top-level one.
BEZEL_TEST(object_slot_equals_default_6725, nested_class_own_nullable_default_spec_8_3) {
    auto r = runAot(R"(
module M;
public class Outer
  public class Inner
    public method Equals(other: Inner? := null): bool => true;
    public override method GetHashCode(): int => 0;
  end Inner;
end Outer;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of class `Inner` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A user base declares `Equals` at another signature and the derived class's
// own-typed `Equals` carries a default: the rule does not depend on the base
// lookup that the in-place class check runs on.
BEZEL_TEST(object_slot_equals_default_6725, class_with_user_base_own_default_spec_8_3) {
    auto r = runAot(R"(
module M;
public class B
  public virtual method Equals(a: int, b: int): bool => true;
  public override method GetHashCode(): int => 0;
end B;
public class D: B
  public override method Equals(a: int, b: int): bool => true;
  public override method Equals(other: D? := null): bool => true;
  public override method GetHashCode(): int => 0;
end D;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of class `D` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §9.2 / §10.5: an enum's own-typed `Equals` with a default.
BEZEL_TEST(object_slot_equals_default_6725, enum_own_typed_default_override_spec_10_5) {
    auto r = runAot(R"(
module M;
public enum Color Red, Green, Blue;
  public override method Equals(other: Color := Color.Red): bool => true;
  public override method GetHashCode(): int => 1;
end Color;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of enum `Color` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §35.2: a choice's own-typed `Equals` with a default, plain spelling.
BEZEL_TEST(object_slot_equals_default_6725, choice_own_typed_default_plain_spec_35_2) {
    auto r = runAot(R"(
module M;
public choice Ch
  P(x: int), Q;
  public method Equals(other: Ch := Ch.Q): bool => true;
  public method GetHashCode(): int => 1;
end Ch;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of choice `Ch` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §9.2: a generic record's own-typed `Equals` (with its own type parameter
// list) carries a default.
BEZEL_TEST(object_slot_equals_default_6725, generic_record_own_typed_default_spec_9_2) {
    auto r = runAot(R"(
module M;
public record G<T>(V: int)
  public method Equals(other: G<T> := new G<T>(0)): bool => V = other.V;
  public method GetHashCode(): int => V;
end G;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "fills the `Object` slot of record `G` and takes no default value: drop the default"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---- control rows: same member names, shapes the rule does not reach ----

// An ordinary class `Equals(Object?)` override with no default still answers
// the slot, and `GetHashCode` is the declared one.
BEZEL_TEST(object_slot_equals_default_6725, class_ordinary_equals_override_no_default_spec_8_3_control) {
    auto r = runAot(R"(
module M;
public class C
  public override method Equals(other: Object?): bool => other <> null;
  public override method GetHashCode(): int => 7;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    Console.WriteLine("a=" + (if c.Equals(c) then 1 else 0).ToString());
    Console.WriteLine("b=" + (if c.Equals(null) then 1 else 0).ToString());
    Console.WriteLine("h=" + c.GetHashCode().ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=1\nb=0\nh=7\n"));
}

// §9.2 / §35.2: an own-typed `Equals` on a record and on a choice with no
// default. A boxed record reaches the same member through `Object.Equals`.
BEZEL_TEST(object_slot_equals_default_6725, record_and_choice_own_typed_no_default_spec_9_2_control) {
    auto r = runAot(R"(
module M;
public record R(X: int)
  public method Equals(other: R): bool => X = other.X;
  public method GetHashCode(): int => X;
end R;
public choice Ch
  P(x: int), Q;
  public method Equals(other: Ch): bool => true;
  public method GetHashCode(): int => 1;
end Ch;
public class Program
  public static method Main(): int
    var r := new R(1);
    var o: Object := r;
    Console.WriteLine("direct=" + (if r.Equals(new R(1)) then 1 else 0).ToString());
    Console.WriteLine("boxed=" + (if o.Equals(new R(1)) then 1 else 0).ToString());
    Console.WriteLine("other=" + (if r.Equals(new R(2)) then 1 else 0).ToString());
    var c := Ch.P(3);
    Console.WriteLine("choice=" + (if c.Equals(Ch.Q) then 1 else 0).ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("direct=1\nboxed=1\nother=0\nchoice=1\n"));
}

// A plain `Equals(other: int := 0)` is not the slot (§8.3: "an Equals taking any
// other type ... fills no slot"), so its default is legal.
BEZEL_TEST(object_slot_equals_default_6725, class_equals_int_default_is_not_slot_spec_8_3_control) {
    auto r = runAot(R"(
module M;
public class C
  public method Equals(other: int := 0): bool => other = 0;
  public override method Equals(other: Object?): bool => other <> null;
  public override method GetHashCode(): int => 7;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    Console.WriteLine("d0=" + (if c.Equals() then 1 else 0).ToString());
    Console.WriteLine("d5=" + (if c.Equals(5) then 1 else 0).ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("d0=1\nd5=0\n"));
}

// A two-parameter `Equals` with a default on its second parameter is not the
// one-parameter slot member, so the default is legal.
BEZEL_TEST(object_slot_equals_default_6725, class_equals_two_params_default_is_not_slot_spec_8_3_control) {
    auto r = runAot(R"(
module M;
public class C
  public method Equals(other: C, flag: bool := false): bool => flag;
  public override method Equals(other: Object?): bool => other <> null;
  public override method GetHashCode(): int => 7;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    Console.WriteLine("f0=" + (if c.Equals(c, false) then 1 else 0).ToString());
    Console.WriteLine("f1=" + (if c.Equals(c, true) then 1 else 0).ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("f0=0\nf1=1\n"));
}

// `ToString` with a defaulted format parameter is not an `Equals` member. The
// zero-argument call is not exercised: it is ambiguous with `Object.ToString()`.
BEZEL_TEST(object_slot_equals_default_6725, class_tostring_format_default_spec_12_2_control) {
    auto r = runAot(R"(
module M;
public class C
  public method ToString(format: string := "G"): string => format;
  public override method Equals(other: Object?): bool => other <> null;
  public override method GetHashCode(): int => 7;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    Console.WriteLine(c.ToString("X"));
    Console.WriteLine(c.ToString("G"));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("X\nG\n"));
}

// `GetHashCode` with a parameter is not the zero-parameter slot; the
// `Object`-typed receiver still reaches the declared `GetHashCode()`.
BEZEL_TEST(object_slot_equals_default_6725, class_gethashcode_param_default_is_not_slot_spec_8_3_control) {
    auto r = runAot(R"(
module M;
public class C
  public method GetHashCode(seed: int := 0): int => seed + 1;
  public override method Equals(other: Object?): bool => other <> null;
  public override method GetHashCode(): int => 7;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine("g5=" + c.GetHashCode(5).ToString());
    Console.WriteLine("slot=" + o.GetHashCode().ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("g5=6\nslot=7\n"));
}

// A static member named `Compare` with a defaulted parameter is not `Equals`
// and fills no slot.
BEZEL_TEST(object_slot_equals_default_6725, class_static_nonequals_default_spec_12_2_control) {
    auto r = runAot(R"(
module M;
public class C
  public static method Compare(other: C? := null): bool => other = null;
  public override method Equals(other: Object?): bool => other <> null;
  public override method GetHashCode(): int => 7;
end C;
public class Program
  public static method Main(): int
    Console.WriteLine("s=" + (if C.Compare() then 1 else 0).ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("s=1\n"));
}
