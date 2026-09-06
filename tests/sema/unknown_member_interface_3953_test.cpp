// Regression tests for #3953 (§11.2/§11.3/§8.1/§17.4, v0.294): a member
// access through a PLAIN interface-typed parameter/local, naming a member
// that is not declared on that interface at all, was accepted by sema with
// no diagnostic and reached codegen's "permissive acceptance" backstop,
// which silently zero-filled the access at runtime — exit 0, no diagnostic,
// wrong value.
//
// THE DEFECT — measured on this fix's parent commit, `bezelc -o out repro.bzl`:
//
//     module M;
//     public interface IFoo
//       method Name(): string;
//     end IFoo;
//     class Box: IFoo
//       public Extra: int := 99;
//       public method Name(): string => "box";
//     end Box;
//     static class F
//       public static method Use(b: IFoo): int
//         return b.Extra;      -- `Extra` is NOT declared on IFoo
//       end Use;
//     end F;
//     class C
//       public static method Main(): int
//         var box := new Box();
//         return F.Use(box);
//       end Main;
//     end C;
//
//   compiled at exit 0 with only an "internal:" stderr note ("permissive
//   acceptance (spec §3 / issue #605) admits this site but no concrete
//   member dispatch exists"), and RAN at exit 0 (returning 0, not 99) —
//   confirmed byte-identical against the identical program with `b: Box`
//   (a CONCRETE receiver), which correctly compiles and returns 99. That
//   asymmetry (concrete type: correct value; interface type: silently 0)
//   is the defect this file pins closed.
//
// ROOT CAUSE: `ExprTC::resolveMemberAccess`'s "no member" gate
// (`src/sema/ExprTypecheck_Calls.cpp`, issue #414's `knownCompleteUserType`)
// fires only for `class`/`record`/`enum`/`choice` receivers, explicitly
// EXCLUDING `TypeDeclKind::Interface` with the stated reason "default/
// inherited interface-member resolution is not complete here". That
// exclusion is stale: `ExprTC::lookupMember` (`src/sema/ExprTypecheck.cpp`)
// already BFS-walks `TypeSymbol::members` AND `TypeSymbol::baseTypes` for
// ANY `TypeSymbol` kind, and both `SymbolCollection::collectTypeDecl` (local
// declarations) and `ModuleLoader`'s importer populate both uniformly for
// EVERY `ast::TypeDeclKind`, interfaces (own members + §11.2 extended-
// interface bases) included. So a member name that survives `lookupMember`
// AND the `lookupExtensionMember` fallback on an interface-typed receiver
// genuinely does not exist on that interface — the same confidence the
// class/record/enum/choice case already has.
//
// THE FIX widens `knownCompleteUserType` (in BOTH `resolveMemberAccess`, the
// `.` form, and `resolveNullCondMember`, the `?.` form — a second, drifting
// spelling of the SAME predicate existed there, confirmed to have the
// identical gap before this fix) to also cover
// `ast::TypeDeclKind::Interface`, and gives the interface case its OWN
// diagnostic wording (mirroring the precedent set by fix #3929/#3952 for
// operator overloading on an interface operand): the generic record/class
// wording says nothing about WHY the member is unreachable, but for an
// interface the natural question is "doesn't the underlying object have
// this member?" — the answer is the §11.2 abstraction boundary, named
// explicitly, with the two remedies that are verified WORKING below:
// narrow to the concrete type (`is`/`as`, §17.4), or declare the member on
// the interface itself (§11.2).
//
// Static interface members (§11.8) are UNAFFECTED by this fix: they are
// already rejected, before ever reaching this gate, by the pre-existing
// §22.12 "static member must be accessed through the type name" check
// (`lookupMember` finds a static member as a real symbol, same as for a
// class), verified by the `static_interface_member_via_instance_rejected`
// control below.
//
// SURFACE MAP (measured before this fix; all six rows compiled at exit 0
// with the identical "permissive acceptance" internal note and a
// zero/degraded runtime result; all six are now clean sema rejections):
//   - member on the implementing class, not the interface (the issue's
//     exact repro)                          -> member_on_implementing_class_
//                                               not_iface_rejected
//   - member on no type at all (a typo)     -> typo_member_rejected
//   - member on a DIFFERENT interface the
//     class also implements                -> member_on_other_implemented_
//                                               iface_rejected
//   - method-call form (`b.Extra()`)        -> method_call_form_rejected
//   - property-read form (an expression-
//     bodied property on the class)         -> property_read_form_rejected
//   - `?.` form (`b?.Extra`)                -> nullcond_form_rejected
//
// FAIL-WITHOUT/PASS-WITH (proven mechanically): with `src/sema/
// ExprTypecheck_Calls.cpp` reverted to this fix's parent, every
// `*_rejected` test below FAILS (the program compiles and runs at exit 0
// instead of being rejected) and every positive control still PASSES
// (unaffected); restoring the fix and relinking flips every `*_rejected`
// test to PASS with no change to the positive controls.
//
// Filters run (no --area): `interface` / `unknown_member` / `member` /
// `dispatch` / `permissive` / `conformance` — see the commit body for exact
// pass counts and which binaries each hit.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Asserts a clean, real-location §11.2 rejection naming `member` and
// `ifaceName`, that it reaches neither codegen's permissive-acceptance
// backstop nor the #2434/#2491 zero-degrade notes, and that it does NOT
// regress to the pre-existing record/class/enum/choice wording (which would
// be a silent drift back toward a message that names the wrong section).
// `program` must be a COMPLETE, self-contained module source.
void expectInterfaceUnknownMemberRejected(const std::string& program,
                                           const char* member,
                                           const char* ifaceName) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string wantNoMember =
        std::string("no member `") + member + "` on interface `" +
        ifaceName + "`";
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, wantNoMember.c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""11.2"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "Narrow to the concrete type first"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`is`/`as`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.4"));
    // Never the pre-existing record/class/enum/choice spelling.
    const std::string oldWording =
        std::string("no member `") + member + "` on type `" + ifaceName +
        "` (\xC2\xA7""11.3, \xC2\xA7""12.1)";
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, oldWording.c_str()));
    // Never reaches codegen's permissive-acceptance backstop or the
    // #2434/#2491 zero-degrade notes (the defect this fix closes).
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "permissive acceptance"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "degraded to a zero"));
}

}  // namespace

// ── The issue's exact repro: member on the implementing class, not IFoo ──

BEZEL_TEST(unknown_member_interface_3953,
           member_on_implementing_class_not_iface_rejected) {
    expectInterfaceUnknownMemberRejected(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public Extra: int := 99; "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    return b.Extra; "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;",
        "Extra", "IFoo");
}

// ── Member exists on no type at all (a genuine typo) ──────────────────────

BEZEL_TEST(unknown_member_interface_3953, typo_member_rejected) {
    expectInterfaceUnknownMemberRejected(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    return b.TotallyBogusMember123(); "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;",
        "TotallyBogusMember123", "IFoo");
}

// ── Member declared on a DIFFERENT interface the class also implements ───

BEZEL_TEST(unknown_member_interface_3953,
           member_on_other_implemented_iface_rejected) {
    expectInterfaceUnknownMemberRejected(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "public interface IBar "
        "  method Extra(): int; "
        "end IBar; "
        "class Box: IFoo, IBar "
        "  public method Name(): string => \"box\"; "
        "  public method Extra(): int => 99; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    return b.Extra(); "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;",
        "Extra", "IFoo");
}

// ── Method-call form ───────────────────────────────────────────────────────

BEZEL_TEST(unknown_member_interface_3953, method_call_form_rejected) {
    expectInterfaceUnknownMemberRejected(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public method Name(): string => \"box\"; "
        "  public method Extra(): int => 99; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    return b.Extra(); "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;",
        "Extra", "IFoo");
}

// ── Property-read form ─────────────────────────────────────────────────────

BEZEL_TEST(unknown_member_interface_3953, property_read_form_rejected) {
    expectInterfaceUnknownMemberRejected(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public property Extra: int => 99; "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    return b.Extra; "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;",
        "Extra", "IFoo");
}

// ── `?.` form — resolveNullCondMember carries the SAME predicate, which had
// the identical gap before this fix (a second, drifting spelling). ────────

BEZEL_TEST(unknown_member_interface_3953, nullcond_form_rejected) {
    auto r = runAot(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public Extra: int := 99; "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo?): int "
        "    return b?.Extra ?? -1; "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Extra` on interface `IFoo`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""11.2"));
}

// ── Positive controls: accesses that MUST still work ──────────────────────

// A member genuinely declared on the interface resolves and dispatches
// correctly (the concrete VALUE, not just "no error").
BEZEL_TEST(unknown_member_interface_3953, real_iface_member_dispatches) {
    auto out = runAotOut(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public Extra: int := 99; "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): string "
        "    return b.Name(); "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    Console.WriteLine(F.Use(box)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, "box");
}

// A member inherited from an EXTENDED interface (§11.2 multiple/transitive
// interface inheritance) resolves through the derived interface's own
// TypeSymbol::baseTypes walk.
BEZEL_TEST(unknown_member_interface_3953, inherited_iface_member_dispatches) {
    auto r = runAot(
        "module M; "
        "public interface IParent method ParentM(): int; end IParent; "
        "public interface IChild: IParent method ChildM(): int; end IChild; "
        "class Impl: IChild "
        "  public method ParentM(): int => 7; "
        "  public method ChildM(): int => 8; "
        "end Impl; "
        "static class F "
        "  public static method Use(c: IChild): int "
        "    return c.ParentM() + c.ChildM(); "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var impl := new Impl(); "
        "    return F.Use(impl); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
}

// §8.1 universal Object members remain reachable through an interface-typed
// receiver (exempted by name from the new gate, same as for class/record/
// enum/choice).
BEZEL_TEST(unknown_member_interface_3953, universal_object_members_clean) {
    auto r = runAot(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    var s := b.ToString(); "
        "    var h := b.GetHashCode(); "
        "    var eq := b.Equals(b); "
        "    var re := Object.ReferenceEquals(b, b); "
        "    if eq and re then return 1; end; "
        "    return 0; "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §22.12 (unaffected by this fix): a STATIC interface member is already
// rejected via instance access before ever reaching the "no member" gate —
// `lookupMember` finds it as a real symbol, exactly like a static class
// member does.
BEZEL_TEST(unknown_member_interface_3953,
           static_interface_member_via_instance_rejected) {
    auto r = runAot(
        "module M; "
        "public interface INumber<T> where T: INumber<T> "
        "  static method Zero(): owned T; "
        "end INumber; "
        "class Meters: INumber<Meters> "
        "  public Value: int := 0; "
        "  public static method Zero(): owned Meters => new Meters(); "
        "end Meters; "
        "static class F "
        "  public static method Use(m: INumber<Meters>): int "
        "    using var z := m.Zero(); "
        "    return 0; "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var mt := new Meters(); "
        "    return F.Use(mt); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "static member `Zero` must be accessed through the type name"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""22.12"));
}

// ── Remedy 1 (verified WORKING): narrow to the concrete type with `is` ────

BEZEL_TEST(unknown_member_interface_3953, remedy_narrow_with_is_works) {
    auto r = runAot(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public Extra: int := 99; "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    if b is Box then "
        "      return b.Extra; "
        "    end; "
        "    return -1; "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ── Remedy 1b (verified WORKING): narrow to the concrete type with `as` ───

BEZEL_TEST(unknown_member_interface_3953, remedy_narrow_with_as_works) {
    auto r = runAot(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public Extra: int := 99; "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    var box := b as Box; "
        "    if box <> null then "
        "      return box.Extra; "
        "    end; "
        "    return -1; "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ── Remedy 2 (verified WORKING): add the member to the interface's own
// declaration, so every conformer is required to provide it. ─────────────

BEZEL_TEST(unknown_member_interface_3953, remedy_declare_on_iface_works) {
    auto r = runAot(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "  readonly property Extra: int; "
        "end IFoo; "
        "class Box: IFoo "
        "  public property Extra: int => 99; "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "static class F "
        "  public static method Use(b: IFoo): int "
        "    return b.Extra; "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return F.Use(box); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ── Regression guard: a §26.5 extension method on an interface receiver is
// found via `lookupExtensionMember` BEFORE this gate is ever consulted, and
// must keep dispatching (already covered by
// generic_interface_extension_dispatch_1276_test.cpp; pinned again here
// alongside the rest of this file's interface-member-access surface). ────

BEZEL_TEST(unknown_member_interface_3953, extension_member_still_dispatches) {
    auto out = runAotOut(
        "module M; "
        "public interface IFoo "
        "  method Name(): string; "
        "end IFoo; "
        "class Box: IFoo "
        "  public method Name(): string => \"box\"; "
        "end Box; "
        "extension IFoo "
        "  public method Greeting(): string => \"hi \" + this.Name(); "
        "end; "
        "static class F "
        "  public static method Use(b: IFoo): string "
        "    return b.Greeting(); "
        "  end Use; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    Console.WriteLine(F.Use(box)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, "hi box");
}
