// #6029 (§12.1 / §12.2 / §8.1 / §28.1, v0.355): two misleading diagnostics on a
// class Object-trio override.
//
// (1) A base class that declares `virtual method ToString(fmt: string): string`
//     and a derived class that declares `override method ToString(): string`.
//     The base class's own member of that name OCCUPIES the name in the
//     derived class (§12.2: "a class's own `Equals(a: K, b: K)` blocks that
//     name in its subclasses as any member does"), so `Object`'s slot is not
//     reachable by an `override` there and there is no virtual `ToString()` to
//     override.  The old diagnostic — "override of `ToString` does not match
//     the base member's parameter count (§12.1)" — named the parameter count,
//     which no spelling of the derived declaration repairs: the only
//     parameter-count repair matches the BASE's member and overrides that
//     instead of the slot.
//
// (2) `override method Equals(owned other: Object?): bool`.  §28.1 admits
//     `owned` on a class type, and `Object` is one ("`Object` is such a class
//     type (§8.1)"), so the §28.1 "Type requirement" has nothing to say about
//     this parameter; the old diagnostic said "`owned` is not admissible on a
//     parameter of type a primitive value type", which is false of `Object?`.
//     The operative fault is §12.1's parameter-modifier match — §8.1 declares
//     `Equals(other: Object?)` with no modifier — and §12.1 reports it.
//
// Both programs stay REJECTED; only the message changes.
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

// (1) §12.1 / §12.2 / §8.1: the derived `override` fills no virtual member —
// the base class's own `ToString(fmt: string)` holds the name — so there is no
// virtual `ToString()` to override.
BEZEL_TEST(object_trio_override_message_6029, tostring_override_of_a_base_format_member_rejected_spec_12_1_12_2) {
    auto r = runAot(R"(
module M;
public class Base
  public constructor() end constructor;
  public virtual method ToString(fmt: string): string => fmt;
end Base;
public class Derived : Base
  public constructor() end constructor;
  public override method ToString(): string => "d";
end Derived;
public class Program
  public static method Main(): int
    var d: Derived := new Derived();
    var o: Object := d;
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "override of `ToString` matches no virtual member"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "there is no virtual `ToString()` to override"));
    // The old, misleading text must be gone: it sent the programmer after the
    // parameter count, whose only repair overrides the base's member.
    BEZEL_EXPECT_FALSE(hasErrorContaining(
        r, "does not match the base member's parameter count"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// (2) §12.1 / §8.1 / §28.1: the `owned` marker on the parameter is a §12.1
// modifier mismatch against `Object.Equals`, not a §28.1 type-requirement
// failure (and `Object?` is not "a primitive value type").
BEZEL_TEST(object_trio_override_message_6029, owned_parameter_on_equals_override_rejected_spec_12_1_28_1) {
    auto r = runAot(R"(
module M;
public class C
  public constructor() end constructor;
  public override method Equals(owned other: Object?): bool => true;
  public override method GetHashCode(): int => 7;
  public override method ToString(): string => "s";
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.Equals(null));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "does not match `Object.Equals`'s parameter modifiers"));
    // The old, misleading text must be gone: `Object` is a class type and
    // §28.1 admits `owned` on one.
    BEZEL_EXPECT_FALSE(hasErrorContaining(
        r, "`owned` is not admissible on a parameter of type a primitive value type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The control: the §8.1 slot overrides themselves are unaffected — a class
// that overrides `ToString()` and `Equals(other: Object?)` at the slot's own
// signature compiles, dispatches through `Object`, and prints exactly.
BEZEL_TEST(object_trio_override_message_6029, slot_overrides_unchanged_spec_8_1) {
    auto r = runAot(R"(
module M;
public class C
  public constructor() end constructor;
  public override method ToString(): string => "s";
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): int => 7;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var o: Object := c;
    Console.WriteLine(o.ToString());
    Console.WriteLine(o.Equals(null));
    Console.WriteLine(o.GetHashCode());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s\nTrue\n7\n");
}

// The §12.2 overload set is unaffected too: a base class that declares
// `ToString(fmt: string)` keeps it, the inherited `Object.ToString()` is still
// reachable on an `Object`-typed receiver, and a derived class may override
// the base's own `ToString(fmt: string)` as before.
BEZEL_TEST(object_trio_override_message_6029, base_format_member_and_its_override_unchanged_spec_12_2) {
    auto r = runAot(R"(
module M;
public class Base
  public constructor() end constructor;
  public virtual method ToString(fmt: string): string => fmt;
end Base;
public class Derived : Base
  public constructor() end constructor;
  public override method ToString(fmt: string): string => "d:" + fmt;
end Derived;
public class Program
  public static method Main(): int
    var b: Base := new Base();
    var o: Object := b;
    Console.WriteLine(o.ToString());
    Console.WriteLine(b.ToString("x"));
    var d: Derived := new Derived();
    Console.WriteLine(d.ToString("y"));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "M.Base\nx\nd:y\n");
}

// The §28.1 "Type requirement" still applies where §12.1 does not decide: an
// `owned` parameter of a NON-`override` method is judged by §28.1 alone.
//
// fix #8118 (§28.1 / §8.1 / §28.5, v0.355) FLIPPED this row. It used to pin
// the REJECTION of a plain `owned other: Object?` parameter with the text
// "`owned` is not admissible on a parameter of type a primitive value type" —
// a statement the spec makes FALSE of `Object` in as many words ("`Object` is
// such a class type (§8.1)"). §28.1 admits the marker there, so the program
// is now ACCEPTED and the row pins the acceptance and the run instead.
BEZEL_TEST(object_trio_override_message_6029, owned_object_parameter_outside_an_override_is_admitted_spec_28_1) {
    auto r = runAot(R"(
module M;
public class C
  public constructor() end constructor;
  public method Take(owned other: Object?): bool => true;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    Console.WriteLine(c.Take(null));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
    // The old, misleading text must be gone: `Object` is a class type and
    // §28.1 admits `owned` on one.
    BEZEL_EXPECT_FALSE(hasErrorContaining(
        r, "`owned` is not admissible on a parameter of type a primitive value type"));
}
