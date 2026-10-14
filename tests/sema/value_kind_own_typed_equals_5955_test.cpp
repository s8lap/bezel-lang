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

// §9.2 (v0.338, #5924 item 1; was a #5955 control that accepted it): "An
// `Equals` taking `Object?` or `Object` is rejected on every value kind at the
// declaration, `override` or plain, the diagnostic naming the own-typed
// spelling." A record's plain `Equals(other: Object?)` is therefore rejected
// (it was accepted, obliging nothing), naming `Equals(other: R)`.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           record_plain_object_equals_rejected_spec_9_2) {
    auto r = runAot(R"(
module M;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: Object?): bool => true;
end R;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("may not take `Object?`") != std::string::npos &&
            d.message.find("Equals(other: R)") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
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

// §8.3 (v0.338, #5955; #6032): a generic choice owning at EVERY instantiation
// has no `Object` slot and carries no plain own-typed obligation. The payload
// `owned v: R` (R an owning class) is owning independently of `T`, so the
// choice is owning at every instantiation (§35.4 rule 1, §35.11). Repro of
// #6032. The program does not call `Equals` (a call of a generic owning
// choice's `Equals` is a separate codegen defect, IR verification failure).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owning_choice_plain_own_typed_equals_no_obligation_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;
choice Res<T> Ok(owned v: R), No;
  public method Equals(o: Res<T>): bool => true;
end Res;
class Program public static method Main(): int
  var a := Res<R>.No;
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

// §8.3 (v0.338, #5955; #6032): a generic choice owning only at SOME
// instantiations keeps the obligation. `owned v: T` is owning at a class
// substitution and value-like at `int` (§35.11: the marker is erased there), and
// §8.3 requires "owning at every instantiation" for the exemption, so the plain
// own-typed `Equals` is rejected at its declaration (line 4, column 3).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_choice_owning_only_at_some_instantiations_keeps_obligation_spec_8_3) {
    auto r = runAot(R"(
module M;
choice Wrap<T> Has(owned v: T), None;
  public method Equals(o: Wrap<T>): bool => true;
end Wrap;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(Wrap)`", 4, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5955; #6032): the `override Equals` spelling on a generic
// owning choice is not exempt: the owning-choice exemption is for the plain
// spelling only (the spec-question remains open whether "its declared Equals"
// covers the override). Rejected at the override declaration (line 5, column 3).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owning_choice_override_equals_stays_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;
choice Res<T> Ok(owned v: R), No;
  public override method Equals(o: Res<T>): bool => true;
end Res;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(
        r, "a type that overrides `Equals`", 5, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5955; #6032; #6049): a generic choice owning at every
// instantiation keeps no obligation under a constraint that a legal witness
// meets. #6049 repro, one row per constraint kind: `Ok(owned v: R, w: T)` is
// owning independently of `T`, so it is owning at every legal instantiation
// (§35.4 rule 1, §35.11). The programs print and do NOT call `Equals` (a call of
// a generic owning choice's `Equals` fails LLVM verification, #6042).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owning_choice_notnull_constraint_no_obligation_spec_8_3) {
    auto r = runAot(R"(
module M;
interface IShape end IShape;
class R public V: int := 0; destructor end destructor; end R;
choice W<T> where T: notnull Ok(owned v: R, w: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  var a := W<R>.No;
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owning_choice_sendable_constraint_no_obligation_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;
choice W<T> where T: sendable Ok(owned v: R, w: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  var a := W<int>.No;
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owning_choice_hashable_constraint_no_obligation_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;
choice W<T> where T: hashable Ok(owned v: R, w: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  var a := W<int>.No;
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owning_choice_new_constraint_no_obligation_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;
choice W<T> where T: new() Ok(owned v: R, w: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  var a := W<int>.No;
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owning_choice_interface_constraint_no_obligation_spec_8_3) {
    auto r = runAot(R"(
module M;
interface IShape end IShape;
class R : IShape public V: int := 0; destructor end destructor; end R;
choice W<T> where T: IShape Ok(owned v: R, w: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  var a := W<R>.No;
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

// #6049 repro, purest case: `Has(owned v: T)` under `where T: class` is owning
// at every legal (class) instantiation. ADMITTED (no owning class witness is
// constructible): no obligation. Declaration only: instantiating `W<R>` with an
// `owned T` payload fails LLVM verification (ICmp on the owned field, present on
// 86894f53 too, a separate codegen defect), so `W` is not instantiated.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owning_choice_class_constraint_no_obligation_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;
choice W<T> where T: class Has(owned v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

// §8.3 (v0.338, #5955): the obligation is imposed only where value-likeness is
// PROVEN (ruling d1). `where T: notnull` is a constraint other than `value`, so
// the choice is ADMITTED (base behaviour). Residual false accept: a legal
// instantiation at `int` makes the payload value-like, which this row does not
// reject (see the commit body).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_owned_t_notnull_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
choice Wrap<T> where T: notnull Has(owned v: T), No;
  public method Equals(o: Wrap<T>): bool => true;
end Wrap;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

// §8.3 (v0.338, #5955): ADMITTED, not proven. Each row constrains a type
// parameter by something other than `value` (a class, an interface, a type
// bound), so the d1 predicate does not prove value-likeness and the choice is
// admitted (base behaviour). Declaration only. Residual false accepts, named in
// the commit body.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_class_bound_no_owned_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
choice W<T> where T: class A1(x: int), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_iface_bound_no_owned_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
interface IShape end IShape;
choice W<X,T> where X: IShape X1(x: int), No;
  public method Equals(o: W<X,T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_class_bound_second_parameter_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
choice W<X,T> where T: class T1(x: int), No;
  public method Equals(o: W<X,T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_class_bound_borrow_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
choice W<T> where T: class Ok(v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_iface_bound_borrow_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
interface IShape end IShape;
choice W<T> where T: IShape Ok(v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_type_bound_borrow_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;
choice W<T> where T: R Ok(v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

// §8.3 (v0.338, #5955): ADMITTED, not proven. An interface bound (IComparable,
// IEquatable, IShape), a type-parameter bound and a `notnull` bound are each a
// constraint other than `value`, so the d1 predicate admits the choice (base
// behaviour). Declaration only. Residual false accepts, named in the commit body.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_icomparable_bound_owned_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
choice W<T> where T: IComparable<T> Ok(owned v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_iequatable_bound_owned_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
choice W<T> where T: IEquatable<T> Ok(owned v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_type_param_bound_owned_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
choice W<T,U> where T: U where U: notnull Ok(owned v: T), No;
  public method Equals(o: W<T,U>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_interface_bound_owned_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
interface IShape end IShape;
choice W<T> where T: IShape Ok(owned v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_record_bound_is_s14_2_error_spec_14_2) {
    auto r = runAot(R"(
module M;
record P public V: int; end P;
choice W<T> where T: P Ok(owned v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    bool s14 = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("may name only a class or an interface") != std::string::npos)
            s14 = true;
    BEZEL_EXPECT_TRUE(s14);
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5955): ADMITTED, not proven. The d1 predicate does no name
// lookup, so a bound naming a module class (`class U`) shadowed by the choice's
// own parameter is not resolved at all; any bound other than `value` is admitted
// (base behaviour). Declaration only.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_type_bound_shadowed_class_name_iface_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
class U public V: int := 0; destructor end destructor; end U;
choice W<T,U> where T: U where U: IComparable<U> Ok(owned v: T), No;
  public method Equals(o: W<T,U>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_type_bound_shadowed_class_name_notnull_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
class U public V: int := 0; destructor end destructor; end U;
choice W<T,U> where T: U where U: notnull Ok(owned v: T), No;
  public method Equals(o: W<T,U>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

// §8.3 (v0.338, #5955): ADMITTED, not proven. A bound naming a class is a
// constraint other than `value`, so the d1 predicate admits the choice (base
// behaviour). Declaration only.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           generic_class_named_bound_owned_payload_admitted_unproven_spec_8_3) {
    auto r = runAot(R"(
module M;
class SomeClass public V: int := 0; destructor end destructor; end SomeClass;
choice W<T> where T: SomeClass Has(owned v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(hasAnyHashDiag(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("run\n"));
}

// // §8.3 (v0.338, #5955 narrow, early-return fix): a record with a plain own-typed
// Equals, a plain GetHashCode and `override Equals(Object?)` is REJECTED on base at
// the `override Equals` declaration (base text). The own-typed branch must not
// return past the trailing `override Equals` check.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           record_plain_gh_override_object_equals_keeps_base_reject_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;

record Rec
  public readonly property Amount: int;
  public constructor(amount: int) Amount := amount; end constructor;
  public method Equals(other: Rec): bool => true;
  public method GetHashCode(): int => 1;
  public override method Equals(o: Object?): bool => true;
end Rec;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "a type that overrides `Equals`", 10, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// // §8.3 (v0.338, #5955 narrow, early-return fix): the enum analogue of the record
// row above: base rejects at the `override Equals` declaration.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           enum_plain_gh_override_object_equals_keeps_base_reject_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;

enum En A, B;
  public method Equals(other: En): bool => true;
  public method GetHashCode(): int => 1;
  public override method Equals(o: Object?): bool => true;
end En;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "a type that overrides `Equals`", 8, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// // §35.2 / §8.3 (v0.338, #5955 narrow, early-return fix): the choice analogue:
// base rejects at the `override Equals` declaration.
BEZEL_TEST(value_kind_own_typed_equals_5955,
           choice_plain_gh_override_object_equals_keeps_base_reject_spec_35_2) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;

choice Ch Ok(x: int), No;
  public method Equals(other: Ch): bool => true;
  public method GetHashCode(): int => 1;
  public override method Equals(o: Object?): bool => true;
end Ch;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "a type that overrides `Equals`", 8, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// // §8.3 (v0.338, #5955 narrow, early-return fix): an owning choice that is exempt for
// its plain own-typed Equals still rejects when it declares `override Equals(Object?)`
// with no GetHashCode (base text, at the `override Equals` declaration).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           exempt_owning_choice_override_object_equals_no_gethashcode_keeps_base_reject_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;

choice Res<T> Ok(owned v: R), No;
  public method Equals(o: Res<T>): bool => true;
  public override method Equals(o: Object?): bool => true;
end Res;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "a type that overrides `Equals`", 7, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// // §8.3 (v0.338, #5955): a choice with an unconstrained payload `Has(v: T)` is not
// owning at every instantiation (int is value-like), so its plain own-typed Equals
// obliges GetHashCode. Proven reject (base accepts).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           unconstrained_plain_T_choice_plain_equals_obliges_gethashcode_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;

choice W<T> Has(v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(W)`", 6, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// // §8.3 (v0.338, #5955): a generic choice with no type-parameter payload is value-like
// at int, so its plain own-typed Equals obliges GetHashCode. Proven reject (base accepts).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           unconstrained_no_T_choice_plain_equals_obliges_gethashcode_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;

choice W<T> A(x: int), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(W)`", 6, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// // §8.3 (v0.338, #5955): `value` is the only proven constraint; a `where T: value`
// payload `Has(v: T)` is value-like at int, so the plain own-typed Equals obliges
// GetHashCode. Proven reject (base accepts).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           value_constrained_plain_T_choice_plain_equals_obliges_gethashcode_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;

choice W<T> where T: value Has(v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(W)`", 6, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}

// // §8.3 (v0.338, #5955): `where T: value` with no type-parameter payload is value-like
// at int, so the plain own-typed Equals obliges GetHashCode. Proven reject (base accepts).
BEZEL_TEST(value_kind_own_typed_equals_5955,
           value_constrained_no_T_choice_plain_equals_obliges_gethashcode_spec_8_3) {
    auto r = runAot(R"(
module M;
class R public V: int := 0; destructor end destructor; end R;

choice W<T> where T: value A(x: int), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasHashObligationDiagAt(r, "declaring `Equals(W)`", 6, 3));
    BEZEL_EXPECT_FALSE(r.ran);
}
