// #5955 (§8.3, v0.338): on a value kind (record, enum, choice) whose
// `Object`-slot `Equals` takes its own type, declared `override` or plain, the
// rule reads both spellings of both members: a plain `Equals` obliges
// `GetHashCode`, and a plain `GetHashCode` discharges the obligation.

#include <cstdint>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasHashObligationDiag(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("GetHashCode` (§8.3)") != std::string::npos &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The reject's position is the Equals member's declLoc, carried in the
// diagnostic text as `<file>.bzl:<line>:<column>: ` (the structured location
// is not filled on the runAot path).
bool hasHashObligationDiagAt(const JitResult& r, const std::string& needle,
                             std::uint32_t line, std::uint32_t column) {
    const std::string at = ".bzl:" + std::to_string(line) + ":" +
                           std::to_string(column) + ": ";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("GetHashCode` (§8.3)") != std::string::npos &&
            d.message.find(needle) != std::string::npos &&
            d.message.find(at) != std::string::npos)
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

// §8.3 (v0.338, #5955): a record's plain own-typed `Equals` with no
// `GetHashCode` is rejected.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           record_plain_own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: R): bool => V = o.V;
end R;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(R)`", 4, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5955): an enum's plain own-typed `Equals` with no
// `GetHashCode` is rejected.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           enum_plain_own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
enum E A, B;
  public method Equals(o: E): bool => int(this) = int(o);
end E;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(E)`", 4, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5955): a choice's plain own-typed `Equals` with no
// `GetHashCode` is rejected (a value-like choice has an Object slot).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           choice_plain_own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
choice K One, Two;
  public method Equals(o: K): bool => true;
end K;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(K)`", 4, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5955): a generic record's own-typed `Equals(G<T>)` obliges
// `GetHashCode` too.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_record_plain_own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
record G<T> public property V: T; public constructor(v: T) V := v; end constructor;
  public method Equals(o: G<T>): bool => true;
end G;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(G)`", 4, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5955): a record nested in a class is held to the same rule
// (the nested walk of #5954 reaches it).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           nested_record_plain_own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
class O
  public record R public property V: int; public constructor(v: int) V := v; end constructor;
    public method Equals(o: R): bool => true;
  end R;
end O;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(R)`", 5, 5));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5955) control: a plain `GetHashCode` discharges a record's
// plain own-typed `Equals`.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           record_plain_own_typed_equals_with_plain_gethashcode_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: R): bool => V = o.V;
  public method GetHashCode(): int => V;
end R;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §8.3 (v0.338, #5955) control: an enum's plain own-typed `Equals` with a
// plain `GetHashCode` is accepted and runs.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           enum_plain_own_typed_equals_with_plain_gethashcode_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
enum E A, B;
  public method Equals(o: E): bool => int(this) = int(o);
  public method GetHashCode(): int => int(this);
end E;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §8.3 (v0.338, #5955) control: a record's `override Equals` taking its own
// type is discharged by a plain `GetHashCode` (both spellings of both
// members).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           record_override_own_typed_equals_with_plain_gethashcode_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public override method Equals(o: R): bool => V = o.V;
  public method GetHashCode(): int => V;
end R;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §8.3 (v0.338, #5955) control: a record's `Equals(other: Object?)` takes no
// own type, so it does not oblige `GetHashCode` under the own-typed rule.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           record_plain_object_equals_obliges_nothing_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: Object?): bool => true;
end R;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §8.3 (v0.338, #5955) control: an owning-classified choice (an `owned` class
// payload, §35.4) has no `Object` slot, so its plain own-typed `Equals` obliges
// nothing (§35.7, §35.11).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           owning_choice_plain_own_typed_equals_obliges_nothing_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;
choice FetchOutcome Fetched(owned response: R), NotModified;
  public method Equals(o: FetchOutcome): bool => true;
end FetchOutcome;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §8.3 (v0.338, #5955) control: a choice's plain own-typed `Equals` with a
// plain `GetHashCode` is accepted and runs (both members are printed).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           choice_plain_own_typed_equals_with_plain_gethashcode_accepted_and_run_spec_8_3) {
    auto r = runAot(R"(
module M;
choice K One, Two;
  public method Equals(o: K): bool => true;
  public method GetHashCode(): int => 4;
end K;
class Program public static method Main(): int
  var a := K.One; var b := K.Two;
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n4\n"));
}

// §8.3 (v0.338, #5955) control: a choice's plain own-typed `Equals` with an
// `override GetHashCode` is accepted and runs (the override discharges the
// obligation as the plain spelling does).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           choice_plain_own_typed_equals_with_override_gethashcode_accepted_and_run_spec_8_3) {
    auto r = runAot(R"(
module M;
choice K One, Two;
  public method Equals(o: K): bool => true;
  public override method GetHashCode(): int => 4;
end K;
class Program public static method Main(): int
  var a := K.One; var b := K.Two;
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n4\n"));
}

// §8.3 (v0.338, #5955) control: a record's plain own-typed `Equals` beside an
// `override GetHashCode` is accepted and runs.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           record_plain_own_typed_equals_with_override_gethashcode_accepted_and_run_spec_8_3) {
    auto r = runAot(R"(
module M;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: R): bool => V = o.V;
  public override method GetHashCode(): int => V;
end R;
class Program public static method Main(): int
  var a := new R(7); var b := new R(7);
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n7\n"));
}

// §8.3 (v0.338, #5955) control: an enum's plain own-typed `Equals` beside an
// `override GetHashCode` is accepted and runs.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           enum_plain_own_typed_equals_with_override_gethashcode_accepted_and_run_spec_8_3) {
    auto r = runAot(R"(
module M;
enum E A, B;
  public method Equals(o: E): bool => int(this) = int(o);
  public override method GetHashCode(): int => int(this) + 10;
end E;
class Program public static method Main(): int
  var a := E.A; var b := E.B;
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n10\n"));
}

// §8.3 (v0.338, #5955) control: an enum's `override Equals` taking its own
// type is discharged by a plain `GetHashCode`, and runs.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           enum_override_own_typed_equals_with_plain_gethashcode_accepted_and_run_spec_8_3) {
    auto r = runAot(R"(
module M;
enum E A, B;
  public override method Equals(o: E): bool => int(this) = int(o);
  public method GetHashCode(): int => int(this) + 10;
end E;
class Program public static method Main(): int
  var a := E.A; var b := E.B;
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n10\n"));
}

// §8.3 (v0.338, #5955) control: a generic record's own-typed `Equals(G<T>)`
// with a plain `GetHashCode` is accepted and runs.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_record_own_typed_equals_with_gethashcode_accepted_and_run_spec_8_3) {
    auto r = runAot(R"(
module M;
record G<T> public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: G<T>): bool => V = o.V;
  public method GetHashCode(): int => V;
end G;
class Program public static method Main(): int
  var a := new G<int>(3); var b := new G<int>(3);
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n3\n"));
}

// §8.3 (v0.338, #5955): a record whose plain own-typed `Equals` is paired with
// a wrong-signature `override GetHashCode(x: int)` is still rejected: the
// wrong-signature override does not discharge the obligation (#5956 rule,
// reach check on the value-kind path).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           record_wrong_signature_gethashcode_does_not_discharge_5956_spec_8_3) {
    auto r = runAot(R"(
module M;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: R): bool => V = o.V;
  public override method GetHashCode(x: int): int => x;
end R;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(R)`", 4, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}
