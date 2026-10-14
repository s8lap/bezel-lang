// Spec-derived regression tests for issue #2765 (owner ruling 2026-08-11,
// landed as the stdlib §2.8/§2.9 findability note in v0.241; carried in
// v0.282).
//
// THE RULE. Stdlib §2.8 (`IComparable<T>`) says, verbatim:
//
//   "The stdlib's primitives and `string` conform to
//    `IComparable<T>`/`IEquatable<T>` at their own type only
//    (`Int32: IComparable<int>`, `String: IComparable<string>`, …); both
//    interfaces are invariant (language spec §12.9), so no
//    `IComparable<Object>`/`IEquatable<Object>` view of them exists —
//    object-keyed equality is `Object.Equals` (language spec §8.3)."
//
// §2.9 repeats it for `IEquatable<T>`. Conformance is NOMINAL and DECLARED,
// invariance makes `IEquatable<string>` and `IEquatable<Object>` unrelated
// types, and `string`/`string?` are distinct types (§7) — so all three
// conversions the issue questioned are unlicensed by standing text and the
// compiler owes a rejection:
//
//   1. `Object` → `IEquatable<Object>`
//   2. `Object` → `IComparable<Object>`
//   3. `string` → `IComparable<string?>`   (the nullable gap)
//
// The ruling records that no split happens: `IEquatable<Object>` IS
// semantically meaningful (stdlib §9.6 declares
// `public pure override method Equals(other: Object?): bool;` on `String`,
// content-based), but that object-keyed equality surface already exists via
// `Object.Equals` dispatch (§8.3), so admitting the interface VIEW would
// invent an undeclared stdlib conformance. No new prohibition on nullable
// type arguments is written either — a USER `class C: IComparable<C?>` is a
// declared Named→Named conformance and stays admissible; it never reaches the
// predicate this fix touches.
//
// WHAT WAS BROKEN, and where. Two separate admissions, one per shape:
//
//   * `ExprTC::primitiveBoxesIntoBuiltinIface` (src/sema/ExprTypecheck.cpp)
//     ended in `return targ->primitive == fromPrim;` with `targ->nullable`
//     never read, and with `Object` — a `PrimitiveType` enumerator only
//     because §4's Object class is a primitive LOOKUP KEY (TypeRef.hpp) —
//     satisfying that test incidentally. This is the predicate the §22.6
//     overload-applicability judgment calls (`implicitlyConvertible`,
//     src/sema/ExprTypecheck_Calls.cpp:179), so it governs the ARGUMENT site.
//   * `ExprTC::checkAssignable`'s `Object → reference-typed` permissive
//     early-return (the #3115 carve-out) admitted the two Object-keyed
//     interface targets at every STORAGE site without consulting the
//     predicate at all.
//
// Both had to be closed: closing only the predicate leaves
// `var e: IEquatable<Object> := o;` accepted, and closing only the
// early-return leaves the argument site accepted. MEASURED on e9bd762, all
// five reject-shapes below compiled clean.
//
// WHAT THE FIX ALSO REMOVES. #2752 had to give the interface-dispatch arm a
// runtime discriminator on BOTH operands, and for `CompareTo` a deliberate
// `abort()` — §4.6 confines ordinal comparison to string operands, so with a
// string receiver and a non-string `Object` argument there is no ordering to
// return. That arm was reachable ONLY through the `IComparable<Object>`
// acceptance; with the conversion rejected it is statically unreachable. See
// tests/codegen/string_builtin_iface_dispatch_2752_test.cpp, whose three
// former `IEquatable<Object>` witnesses are re-spelled on the still-reachable
// `Object.Equals(Object?)` path in the same commit.
//
// SCOPE — what this fix must NOT touch, pinned by the accept-controls below:
//   * `string` → `IEquatable<string>` / `IComparable<string>` (stdlib §9.6's
//     declared conformance) and the boxed-primitive tower `int`/`bool`
//     (§9.1/§9.4).
//   * `Object` → any OTHER reference target: `string`, a user class, a user
//     interface, and `IEquatable<string>` (the unchecked downcast the #3115
//     comment deliberately left permissive — tests/codegen/
//     method_group_iface_string_2766_test.cpp now relies on it).
//   * a USER interface named `IEquatable`/`IComparable` (`moduleName`
//     non-empty — the #3022 discriminator), including at `<Object>`.
//   * the `null` literal into a nullable interface slot (§5.1's nullable
//     conversion), and the §7 diagnostic for a genuinely nullable `Object?`
//     source at a non-nullable target — NOT re-routed to the new message.
//
// BRANCH ORDER. This fix adds a classify-and-report arm ahead of an existing
// permissive return, so every test asserts on SPECIFIC diagnostic TEXT rather
// than `hasErrors()`: the reject-shapes that must carry the new §2.8/§2.9
// wording assert it, and the shapes that must KEEP the plain §5.1 or the §7
// wording assert the new wording is ABSENT. A regression that re-routes a
// diagnostic is otherwise silent.
//
// Authority: stdlib §2.8/§2.9, §9.1, §9.4, §9.6 (v0.282); spec §4.6, §5.1,
// §7, §8.3, §12.9 (v0.282).

#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §5.1 "not implicitly convertible" stem shared by every rejection in
// checkAssignable. Necessary but NOT sufficient — see kObjectView.
constexpr const char* kNotConvertible =
    "has a type that is not implicitly convertible to the target type";

// The #2765 tail, emitted ONLY for the Object-keyed builtin-interface targets.
// Its presence/absence is the branch-order discriminator.
constexpr const char* kObjectView = "view of them exists (stdlib §2.8/§2.9)";

// The pre-existing §7 nullable-source diagnostic. Must survive unchanged for a
// genuinely nullable `Object?` source at a non-nullable target.
constexpr const char* kParagraph7 =
    "is nullable but the target type is non-nullable";

}  // namespace

// =====================================================================
// REJECT — the shapes the 2026-08-11 ruling names. Each compiled CLEAN on
// e9bd762.
// =====================================================================

// #2765 (1): `Object` → `IEquatable<Object>`, the issue body's own example.
// Admitted pre-fix by checkAssignable's `Object → reference` early-return.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           object_slot_iequatable_object_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var v: Object := \"oo\"; "
        "  var got: IEquatable<Object> := v; "
        "  if got.Equals(v) then return 7; end; "
        "  return 3; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(hasError(r, "no `IEquatable<Object>` view of them exists"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§12.9)"));
    BEZEL_EXPECT_TRUE(hasError(r, "`Object.Equals`"));
}

// #2765 (2): `Object` → `IComparable<Object>`. Ruled together with (1); the
// message names the interface actually written, so a copy-paste that reports
// `IEquatable` here would fail.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           object_slot_icomparable_object_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var v: Object := \"q4\"; "
        "  var got: IComparable<Object> := v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(hasError(r, "no `IComparable<Object>` view of them exists"));
}

// #2765: the `Object?` type-argument spelling. `IEquatable<Object?>` has no
// more of a declared conformance than `IEquatable<Object>` does, and the
// message spells the type argument as written.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           object_slot_iequatable_nullable_object_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var v: Object := \"q4\"; "
        "  var got: IEquatable<Object?> := v; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "no `IEquatable<Object?>` view of them exists"));
}

// #2765: a NULLABLE interface slot is covered too. The source is a genuinely
// nullable `Object?` PARAMETER (a `var o: Object? := "x";` local would be
// null-narrowed non-null by §7.4 flow narrowing and would not test this).
// Pre-fix the early-return's `(!from->nullable || to->nullable)` guard admitted
// it; the fix repeats that exact guard, so this shape is intercepted and the
// §7-only shape below is not.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           nullable_object_source_into_nullable_iface_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method G(o: Object?): int "
        "    var e: IEquatable<Object>? := o; "
        "    if e is null then return 1; end; "
        "    return 0; "
        "  end G; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.G(\"x\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kObjectView));
}

// #2765: the ARGUMENT site with an `Object` source. This is the site that
// `checkAssignable`'s early-return does NOT guard on its own — §22.6
// applicability runs `implicitlyConvertible` (ExprTypecheck_Calls.cpp:179),
// which reaches `primitiveBoxesIntoBuiltinIface` directly. A fix that only
// touched checkAssignable would leave this accepted.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           object_arg_iequatable_object_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method Take(c: IEquatable<Object>): int "
        "    return 1; "
        "  end Take; "
        "end F; "
        "class C public static method Main(): int "
        "  var v: Object := \"z\"; "
        "  return F.Take(v) - 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(hasError(r, kObjectView));
}

// #2765 (3): the nullable gap — `string` → `IComparable<string?>`. stdlib §9.6
// declares `CompareTo(other: string)`, never `CompareTo(other: string?)`;
// `IComparable<T>` is invariant (§12.9) and `string`/`string?` are distinct
// types (§7). Pre-fix `primitiveBoxesIntoBuiltinIface` compared
// `targ->primitive == fromPrim` with `targ->nullable` unread.
//
// This one is rejected by the PREDICATE, not the Object arm, so it carries the
// plain §5.1 message — asserted explicitly, so a future broadening that routes
// it through the Object arm is caught.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           string_icomparable_nullable_string_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"q4\"; "
        "  var got: IComparable<string?> := s; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(!hasError(r, kObjectView));
}

// #2765: the same nullable gap on the numeric tower — `int` →
// `IEquatable<int?>`. stdlib §9.1 declares `Int32: IComparable<int>,
// IEquatable<int>`; there is no `int?` conformance. Proves the predicate's
// nullable check is not string-specific.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           int_iequatable_nullable_int_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var got: IEquatable<int?> := 7; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(!hasError(r, kObjectView));
}

// #2765: the nullable gap at the ARGUMENT site — the predicate's other caller.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           string_arg_icomparable_nullable_string_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method Take(c: IComparable<string?>, s: string): int "
        "    return s.Length; "
        "  end Take; "
        "end F; "
        "class C public static method Main(): int "
        "  var s := \"q4\"; "
        "  return F.Take(s, s); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
}

// =====================================================================
// BRANCH-ORDER CONTROLS — shapes that were ALREADY rejected and must keep
// their OWN diagnostic. The new arm sits ahead of the permissive return, so a
// mis-scoped predicate would swallow these.
// =====================================================================

// #2765 control: `string` → `IEquatable<Object>` (a STATICALLY string source,
// unlike the issue body's `Object` source) was already rejected on e9bd762 —
// the predicate's `targ->primitive == fromPrim` failed and the trailing
// concrete-primitive ↔ concrete-class gate reported §5.1. It must STAY on that
// path: the Object arm is keyed on the SOURCE being `Object`, and if this
// shape started carrying the §2.8/§2.9 tail the arm would have been widened
// past its ruling.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           string_source_iequatable_object_keeps_plain_diagnostic) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"q4\"; "
        "  var got: IEquatable<Object> := s; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(!hasError(r, kObjectView));
}

// #2765 control / stdlib §9.4: `bool` → `IComparable<bool>` — `Boolean` lists
// `IEquatable<bool>` only, no `IComparable`. Rejected before and after, with
// the plain §5.1 message.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           bool_icomparable_bool_keeps_plain_diagnostic) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var got: IComparable<bool> := true; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(!hasError(r, kObjectView));
}

// #2765 control / §7: the `null` literal (typed `Object?`) into a NON-nullable
// `IEquatable<Object>` never reached the permissive early-return, so it kept
// the §7 nullable diagnostic — and must keep it. The fix repeats the
// early-return's own `(!from->nullable || to->nullable)` guard precisely so
// this paragraph number does not move.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           null_literal_into_nonnullable_iface_keeps_paragraph_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var got: IEquatable<Object> := null; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParagraph7));
    BEZEL_EXPECT_TRUE(!hasError(r, kObjectView));
}

// #2765 control / §7: a genuinely nullable `Object?` source at a NON-nullable
// `IEquatable<Object>` target — same reasoning, non-literal source.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           nullable_object_source_into_nonnullable_iface_keeps_paragraph_7) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method G(o: Object?): int "
        "    var e: IEquatable<Object> := o; "
        "    return 0; "
        "  end G; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.G(\"x\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParagraph7));
    BEZEL_EXPECT_TRUE(!hasError(r, kObjectView));
}

// =====================================================================
// ACCEPT-CONTROLS — the conversions the ruling leaves alone. A false REJECT
// here is the hazard the issue body warned about ("narrowing an acceptance is
// the false-reject hazard").
// =====================================================================

// #2765 control / stdlib §9.6: the DECLARED conformance
// (`class String: …, IComparable<string>, IEquatable<string>`).
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           string_iequatable_string_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"q4\"; "
        "  var got: IEquatable<string> := s; "
        "  return (if got.Equals(\"q4\") then 0 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNotConvertible));
}

BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           string_icomparable_string_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"mmm\"; "
        "  var got: IComparable<string> := s; "
        "  return (if got.CompareTo(\"nnn\") < 0 then 0 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNotConvertible));
}

// #2765 control / stdlib §9.1: the boxed-primitive tower. The predicate's new
// `Object` rejection is keyed on the SOURCE primitive, so the numeric shapes
// are untouched.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           int_iequatable_and_icomparable_int_accepted) {
    auto r1 = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var got: IEquatable<int> := 7; "
        "  return (if got.Equals(7) then 0 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r1, kNotConvertible));
    auto r2 = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var got: IComparable<int> := 5; "
        "  return (if got.CompareTo(3) > 0 then 0 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r2, kNotConvertible));
}

// #2765 control / stdlib §9.4: `bool` → `IEquatable<bool>`, the one interface
// `Boolean` declares.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           bool_iequatable_bool_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var got: IEquatable<bool> := true; "
        "  return (if got.Equals(true) then 0 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNotConvertible));
}

// #2765 control: `Object` → `IEquatable<string>` — the unchecked downcast the
// #3115 comment deliberately left on the permissive path. The new arm is keyed
// on the TYPE ARGUMENT being `Object`, so this must stay accepted.
// tests/codegen/method_group_iface_string_2766_test.cpp's
// one_cached_thunk_three_receivers depends on it.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           object_source_into_iequatable_string_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"zz\"; "
        "  var o: Object := s; "
        "  var e: IEquatable<string> := o; "
        "  return (if e.Equals(\"zz\") then 0 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNotConvertible));
}

// fix #6023 (§5.1 "Reference conversions", §4.6, v0.338) FLIPPED this row. It
// pinned `Object` → a USER interface and `Object` → `string` as ACCEPTED;
// both are downcasts §5.1 does not list and both are now rejected, once per
// site, with the general downcast wording — never #2765's. (`Object` →
// `IEquatable<string>`, the row above, stays accepted: the checked `is`/`as`
// spelling does not work for a synthesized built-in interface yet, #6073.)
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           object_into_user_interface_and_string_rejected_as_downcast_spec_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IThing method Ping(): int; end IThing; "
        "public class Thing: IThing "
        "  public constructor() end constructor; "
        "  public method Ping(): int return 5; end Ping; "
        "end Thing; "
        "class C public static method Main(): int "
        "  using var t := new Thing(); "
        "  var o: Object := t; "
        "  var i: IThing := o; "
        "  var s := \"hi\"; "
        "  var o2: Object := s; "
        "  var s2: string := o2; "
        "  return i.Ping() - 5 + s2.Length - 2; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "`Object` to `IThing` is a downcast"));
    BEZEL_EXPECT_TRUE(hasError(r, "`Object` to `string` is a downcast"));
    BEZEL_EXPECT_TRUE(!hasError(r, kObjectView));
}

// #2765 control / #3022: a USER interface named `IEquatable`, instantiated at
// `Object`. `declaration->moduleName` is non-empty for a user TypeSymbol, so
// neither the predicate nor the new arm may fire — the ruling is about the
// stdlib's conformances, not about the identifier.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           user_declared_iequatable_at_object_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IEquatable<T> method Equals(other: T): bool; "
        "end IEquatable; "
        "public class Holder: IEquatable<Object> "
        "  public constructor() end constructor; "
        "  public override method Equals(other: Object?): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var e: IEquatable<Object> := h; "
        "  return (if e.Equals(h) then 0 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(!hasError(r, kObjectView));
}

// #2765 control / §5.1: the `null` literal into a NULLABLE interface slot is
// §5.1's nullable conversion and stays accepted.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           null_literal_into_nullable_iface_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var got: IEquatable<Object>? := null; "
        "  return (if got is null then 0 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(!hasError(r, kParagraph7));
}

// =====================================================================
// AOT WITNESSES — the fork-isolated compile + link + native exec pipeline,
// the strict oracle. Two rejections and one acceptance that must still RUN.
// =====================================================================

// #2765: the issue body's program is rejected end-to-end. On e9bd762 it
// compiled, linked, and exited 7.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           object_slot_iequatable_object_rejected_by_aot) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var v: Object := \"oo\"; "
        "  var got: IEquatable<Object> := v; "
        "  if got.Equals(v) then return 7; end; "
        "  return 3; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// #2765: the nullable gap, end-to-end.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           string_icomparable_nullable_string_rejected_by_aot) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"q4\"; "
        "  var got: IComparable<string?> := s; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// #2765 accept-control, end-to-end: the declared §9.6 conformance and the
// `Object` → `IEquatable<string>` downcast both still compile AND produce the
// right runtime answers (0 = both correct). A sema-only accept-control cannot
// catch a fix that keeps sema happy but breaks the boxing lowering.
BEZEL_TEST(sema_primitive_boxes_into_builtin_iface_2765,
           declared_conformances_still_run_under_aot) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"zz\"; "
        "  var e1: IEquatable<string> := s; "
        "  var o: Object := s; "
        "  var e2: IEquatable<string> := o; "
        "  var c: IComparable<string> := s; "
        "  var n := 0; "
        "  if not e1.Equals(\"zz\") then n := n + 1; end; "
        "  if not e2.Equals(\"zz\") then n := n + 2; end; "
        "  if c.CompareTo(\"zz\") <> 0 then n := n + 4; end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
