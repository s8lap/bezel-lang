// Regression tests for #7973 (§8.1, v0.350 / §26.5, v0.350): an undeclared
// member on a plain `Object`-typed receiver was ACCEPTED (the stale "#605
// permissive acceptance"), and codegen then degraded the access to a
// zero-initialised slot (#2477 / #2547 / #2434), so the program compiled,
// ran, and produced a WRONG VALUE.
//
// §8.1 (v0.350) declares the root `Object`:
//
//     public class Object
//       public method GetType(): Type;
//       public virtual method Equals(other: Object?): bool;
//       public virtual method GetHashCode(): int;
//       public virtual method ToString(): string;
//       public static method ReferenceEquals(a: Object?, b: Object?): bool;
//     end Object;
//
// and says of that declaration: "The declaration above defines the public
// surface." §26.5 resolves a member against the receiver's DECLARED type, so
// `o.Bogus()` names no member of it and is a compile error.
//
// The SCOPE of the refusal is the root `Object` BY CATEGORY (a
// `Kind::Primitive` / `primitive == Object` TypeRef), never by the spelling:
// §2.2 + §8.1 make every other `Object` a DIFFERENT name. The rows below pin
// both directions — the root's surface is closed, and every other `Object`
// (a user's generic `Object<T>`, a class, an interface, a type parameter)
// keeps its own resolution.
//
// MEASURED on the pre-fix base (this lane's base `48353f2b`): every reject row
// below compiled and exited 0, with only an `internal: codegen cannot
// dispatch/read member '...' on receiver of type 'Object': permissive
// acceptance (spec #3 / issue #605)` warning (and, for the read rows, a
// zero-initialised slot). Every control row below compiled and ran with the
// same output before and after the fix.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The refusal's own wording. §8.1's surface list is transcribed from the
// spec's declaration block, so a change to either side shows up here.
const char* kNoMember = "no member `Bogus` on type `Object`";

// §3.4's static-qualifier wording — the OTHER path for the same name.
const char* kStaticNoMember =
    "type 'Object' has no static method or property 'Bogus' (§3.4)";

// §8.7 / §28.2's class-only wording for `Dispose`.
const char* kDisposeClassOnly = "no member `Dispose` on type `Object`";

// §8.2's reflection gate.
const char* kGetTypeNotImplemented = "reflection is not yet implemented";

std::string mainBody(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// The reject rows — the root `Object`'s surface is CLOSED.
// ---------------------------------------------------------------------------

// The issue's own repro shape: an `Object` local bound from a Task, then an
// undeclared METHOD call.
BEZEL_TEST(object_closed_surface_7973, method_call_on_object_local_is_rejected) {
    auto r = runAot(
        "module M; import System; import System.Threading.Tasks; "
        "class C public static method Main(): int "
        "  using var t := Task.FromResult(2); var o: Object := t; o.Bogus(); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoMember));
}

// The PROPERTY form — a bare read of an undeclared member on the same
// receiver. Before the fix this compiled and degraded the slot to zero
// (the #2547 / #2434 warnings).
BEZEL_TEST(object_closed_surface_7973, property_read_on_object_local_is_rejected) {
    auto r = runAot(mainBody("var o: Object := 5; var x := o.Bogus;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoMember));
}

// The `?.` mirror over a nullable `Object` (§7.5) — the null-conditional
// spelling resolves its member against the same declared type, so it must be
// refused the same way.
BEZEL_TEST(object_closed_surface_7973, null_conditional_on_object_is_rejected) {
    auto r = runAot(mainBody("var o: Object? := 5; o?.Bogus();"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoMember));
}

// The `Object` PARAMETER shape — the receiver's type comes from a parameter's
// declared type, not from a local's annotation, and the surface is the same
// (§8.1 names the type, not the binding).
BEZEL_TEST(object_closed_surface_7973, method_call_on_object_parameter_is_rejected) {
    auto r = runAot(
        "module M; class C "
        "  public method M(o: Object): int var s := o.Bogus(); return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoMember));
}

// The force-unwrap form `o!.Bogus()` on an `Object?` receiver (§7.4) — the
// unwrapped receiver is the root `Object`, so the surface is the same.
BEZEL_TEST(object_closed_surface_7973, force_unwrap_on_object_is_rejected) {
    auto r = runAot(mainBody("var o: Object? := 5; var x := o!.Bogus;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoMember));
}

// ---------------------------------------------------------------------------
// The control rows — the root `Object` still IS the root, and everything else
// keeps its own resolution.
// ---------------------------------------------------------------------------

// The §8.1 members stay legal on an `Object` receiver and read the data.
BEZEL_TEST(object_closed_surface_7973, section_8_1_members_still_bind) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  Console.WriteLine(o.ToString()); "
        "  Console.WriteLine(o.Equals(o)); "
        "  Console.WriteLine(o.GetHashCode()); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\nTrue\n5\n");
}

// §26.5 rule 2: a member an `extension Object` block provides still binds,
// through both `.` and `?.`, and is NOT swallowed by the closed-surface
// refusal.
BEZEL_TEST(object_closed_surface_7973, extension_on_object_still_binds) {
    auto r = runAot(
        "module M; "
        "extension Object public method Tag(): int => 7; end; "
        "class C public static method Main(): int "
        "  var o: Object := 5; var p: Object? := 6; "
        "  Console.WriteLine(o.Tag()); "
        "  Console.WriteLine(p?.Tag() ?? 0); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n7\n");
}

// THE SCOPE ROW (the one a spelling-keyed refusal gets wrong). §8.1: "a
// generic `Object<T>` is a different name (§2.2)". A user's generic
// `Object<T>` is an ordinary user type with its own members, and the
// closed-surface refusal must not touch it — neither its own member nor a
// member it does not declare (which the ordinary §11.3 / §12.1 "no member"
// reject owns).
BEZEL_TEST(object_closed_surface_7973, user_generic_object_keeps_its_own_members) {
    auto r = runAot(
        "module M; "
        "public class Object<T> public method Hi(): int => 7; end Object; "
        "class C public static method Main(): int "
        "  using var o := new Object<int>(); "
        "  Console.WriteLine(o.Hi()); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// A CLASS receiver keeps its own members.
BEZEL_TEST(object_closed_surface_7973, class_receiver_member_still_binds) {
    auto r = runAot(
        "module M; "
        "class Cell public method Get(): int => 1; end Cell; "
        "class C public static method Main(): int "
        "  using var c := new Cell(); "
        "  Console.WriteLine(c.Get()); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// An INTERFACE receiver keeps its own members: §8.1's surface is `Object`'s,
// not every receiver's.
BEZEL_TEST(object_closed_surface_7973, interface_receiver_member_still_binds) {
    auto r = runAot(
        "module M; interface IShape method Area(): int; end; "
        "class Sq : IShape "
        "  public method Area(): int => 4; "
        "  destructor end destructor; "
        "end Sq; "
        "class C public static method Main(): int "
        "  using var s: IShape := new Sq(); "
        "  Console.WriteLine(s.Area()); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}

// A TYPE-PARAMETER receiver keeps the universal members (§8.1): a type
// parameter is not the root `Object`, so the closed surface does not apply.
BEZEL_TEST(object_closed_surface_7973, type_parameter_receiver_still_binds) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Tag(5)); "
        "    return 0; "
        "  end Main; "
        "  public static method Tag<T>(x: T): string => x.ToString(); "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// §12.7's narrowing: inside `if o is Derived then`, the carrier's static type
// is the NARROWED type, so the narrowed member resolves against `Derived`
// and not against the root `Object`. The refusal must not fire here.
BEZEL_TEST(object_closed_surface_7973, narrowed_carrier_member_still_binds) {
    auto r = runAot(
        "module M; "
        "public class Derived public method Extra(): int => 2; end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var o: Object := d; "
        "  if o is Derived then Console.WriteLine(o.Extra()); end; "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// The §3.4 STATIC path is a different question — a type-qualified reference
// (§27.6), not a member of a value — and keeps its own single diagnostic.
BEZEL_TEST(object_closed_surface_7973, static_qualifier_keeps_its_own_diagnostic) {
    auto r = runAot(mainBody("var x := Object.Bogus();"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStaticNoMember));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoMember));
}

// `Dispose` is NOT one of `Object`'s members (§8.7, §28.2), and that rule
// draws its OWN message — the §8.1 closed-surface wording must not shadow
// it.
BEZEL_TEST(object_closed_surface_7973, dispose_keeps_its_own_diagnostic) {
    auto r = runAot(mainBody("var o: Object := 5; o.Dispose();"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kDisposeClassOnly));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoMember));
}

// `GetType` IS on §8.1's surface, and §8.2's reflection gate draws its own
// message for it — the closed-surface refusal must not shadow that either.
BEZEL_TEST(object_closed_surface_7973, get_type_keeps_its_own_diagnostic) {
    auto r = runAot(mainBody("var o: Object := 5; var g := o.GetType();"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kGetTypeNotImplemented));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoMember));
}
