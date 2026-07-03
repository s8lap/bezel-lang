// Spec-derived sema tests for §11.8 (v0.176, #807) — static interface
// members: the conformance rule and the T.Member / constraint-operator
// dispatch typing.
//
// Authority: spec §11.8 (v0.176) — a class/record declaring `: I<Self>` where
// I has static members must supply matching statics (the §11.2 exact-match
// rule applied to statics: same name/arity/param types/modifiers/return
// including `owned`; operators match by token + signature). In a generic body
// with `where T: I<T>`, `T.Member(...)` calls the interface's static member
// and binary operators on T-typed operands resolve through the constraint's
// static operators. Outside such a constraint, T.Member stays an error.
// stdlib §2.19 supplies INumber/ISteppable/IParsable with compiler-provided
// primitive conformances (numeric tower → all three; char → ISteppable +
// IParsable only; string → none).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasErrorContaining(const bezel::test::SemaResult& r,
                        std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A user static interface in the spec §11.8 shape (recurrence-constrained).
const char* kINum =
    "public interface INum<T> where T: INum<T> "
    "  static method Zero(): T; "
    "  static operator +(a: T, b: T): T; "
    "end INum; ";

// A fully conforming class for kINum.
const char* kMeters =
    "public class Meters: INum<Meters> "
    "  public Value: int; "
    "  public constructor(v: int) this.Value := v; end; "
    "  public static method Zero(): Meters => new Meters(0); "
    "  public static method operator +(a: Meters, b: Meters): Meters => "
    "    new Meters(a.Value + b.Value); "
    "end Meters; ";

}  // namespace

// ── §11.8: conformance — the recurrence + statics accept/reject matrix ─────

// The self-referential conformance `class Meters: INum<Meters>` under the
// recurrence constraint `where T: INum<T>` resolves (no ordering false-reject).
BEZEL_TEST(static_iface_conformance_807, recurrence_conformance_accepted) {
    auto r = parseSemaAll(std::string("module M; ") + kINum + kMeters);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Missing static method → conformance error NAMING the member and §11.8.
BEZEL_TEST(static_iface_conformance_807, missing_static_method_named) {
    auto r = parseSemaAll(
        std::string("module M; ") + kINum +
        "public class Broken: INum<Broken> "
        "  public static method operator +(a: Broken, b: Broken): Broken => a; "
        "end Broken;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "Zero"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
}

// Missing static operator → conformance error naming `operator +`.
BEZEL_TEST(static_iface_conformance_807, missing_static_operator_named) {
    auto r = parseSemaAll(
        std::string("module M; ") + kINum +
        "public class Broken: INum<Broken> "
        "  public static method Zero(): Broken => new Broken(); "
        "end Broken;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "operator +"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
}

// Wrong signature (return type differs after substitution) → rejected.
BEZEL_TEST(static_iface_conformance_807, wrong_return_type_rejected) {
    auto r = parseSemaAll(
        std::string("module M; ") + kINum +
        "public class Broken: INum<Broken> "
        "  public static method Zero(): int => 0; "
        "  public static method operator +(a: Broken, b: Broken): Broken => a; "
        "end Broken;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "different signature"));
}

// Wrong arity → rejected as a signature mismatch (§11.2 rule for statics).
BEZEL_TEST(static_iface_conformance_807, wrong_arity_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IFac<T> where T: IFac<T> "
        "  static method Make(seed: int): T; "
        "end IFac; "
        "public class W: IFac<W> "
        "  public static method Make(): W => new W(); "
        "end W;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "different signature"));
}

// The `owned` return modifier participates in the exact-match rule (§11.2):
// an implementation dropping the interface's `owned` is a mismatch.
BEZEL_TEST(static_iface_conformance_807, owned_return_mismatch_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IFac<T> where T: IFac<T> "
        "  static method Make(): owned T; "
        "end IFac; "
        "public class W: IFac<W> "
        "  public static method Make(): W => new W(); "
        "end W;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "different signature"));
}

// An INSTANCE method of the right name does not satisfy a static member.
BEZEL_TEST(static_iface_conformance_807, instance_method_does_not_satisfy) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IZ<T> where T: IZ<T> "
        "  static method Zero(): T; "
        "end IZ; "
        "public class W: IZ<W> "
        "  public method Zero(): W => this; "
        "end W;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
}

// RECORD conformance: a record supplies the statics — accepted.
BEZEL_TEST(static_iface_conformance_807, record_conformance_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IZ<T> where T: IZ<T> "
        "  static method Zero(): T; "
        "end IZ; "
        "public record Pt(x: int): IZ<Pt> "
        "  public static method Zero(): Pt => new Pt(0); "
        "end Pt;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// RECORD conformance: a record missing the static — rejected.
BEZEL_TEST(static_iface_conformance_807, record_missing_static_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IZ<T> where T: IZ<T> "
        "  static method Zero(): T; "
        "end IZ; "
        "public record Pt(x: int): IZ<Pt> end Pt;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
}

// A static interface member creates NO instance obligation: the conforming
// class needs no instance `Zero` (would otherwise trip §12.5's abstract check).
BEZEL_TEST(static_iface_conformance_807, no_instance_obligation_from_static) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IZ<T> where T: IZ<T> "
        "  static method Zero(): T; "
        "  method Scale(k: int): int; "
        "end IZ; "
        "public class W: IZ<W> "
        "  public static method Zero(): W => new W(); "
        "  public method Scale(k: int): int => k; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── stdlib §2.19: the builtin interfaces + compiler-provided conformances ──

// A user class conforming to the stdlib INumber<T> is accepted (the §25.2
// comparison-group rule requires the class to complete `< > <= >=`).
BEZEL_TEST(static_iface_conformance_807, builtin_inumber_user_conformance) {
    auto r = parseSemaAll(
        "module M; "
        "public class Meters: INumber<Meters> "
        "  public Value: int; "
        "  public constructor(v: int) this.Value := v; end; "
        "  public static method Zero(): Meters => new Meters(0); "
        "  public static method One(): Meters => new Meters(1); "
        "  public static method operator +(a: Meters, b: Meters): Meters => "
        "    new Meters(a.Value + b.Value); "
        "  public static method operator -(a: Meters, b: Meters): Meters => "
        "    new Meters(a.Value - b.Value); "
        "  public static method operator *(a: Meters, b: Meters): Meters => "
        "    new Meters(a.Value * b.Value); "
        "  public static method operator /(a: Meters, b: Meters): Meters => "
        "    new Meters(a.Value / b.Value); "
        "  public static method operator <(a: Meters, b: Meters): bool => "
        "    a.Value < b.Value; "
        "  public static method operator >(a: Meters, b: Meters): bool => "
        "    a.Value > b.Value; "
        "  public static method operator <=(a: Meters, b: Meters): bool => "
        "    a.Value <= b.Value; "
        "  public static method operator >=(a: Meters, b: Meters): bool => "
        "    a.Value >= b.Value; "
        "end Meters;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A user class declaring `: INumber<Self>` but missing `One` is rejected.
BEZEL_TEST(static_iface_conformance_807, builtin_inumber_missing_one) {
    auto r = parseSemaAll(
        "module M; "
        "public class Meters: INumber<Meters> "
        "  public static method Zero(): Meters => new Meters(); "
        "end Meters;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "One"));
}

// §2.19 conformance table at the call site: `string` conforms to none.
BEZEL_TEST(static_iface_conformance_807, string_not_inumber) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method Add2<T>(a: T, b: T): T where T: INumber<T> "
        "    return a + b; end Add2; "
        "end F; "
        "class C public static method Main(): int "
        "  var s := F.Add2(\"a\", \"b\"); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not satisfy the constraint"));
}

// §2.19: `char` conforms to ISteppable but NOT to INumber.
BEZEL_TEST(static_iface_conformance_807, char_steppable_not_inumber) {
    auto ok = parseSemaAll(
        "module M; static class F "
        "  public static method Nxt<T>(v: T): T where T: ISteppable<T> "
        "    return T.Step(v, 1L); end Nxt; "
        "end F; "
        "class C public static method Main(): int "
        "  var c := F.Nxt('a'); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(ok.diagnostics.hasErrors());
    auto bad = parseSemaAll(
        "module M; static class F "
        "  public static method Add2<T>(a: T, b: T): T where T: INumber<T> "
        "    return a + b; end Add2; "
        "end F; "
        "class C public static method Main(): int "
        "  var c := F.Add2('a', 'b'); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(bad.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(bad, "does not satisfy the constraint"));
}

// §2.19: `bool` conforms to none of the three.
BEZEL_TEST(static_iface_conformance_807, bool_not_parsable_constraint) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method P<T>(s: string): T where T: IParsable<T> "
        "    return T.Parse(s); end P; "
        "end F; "
        "class C public static method Main(): int "
        "  var b := F.P<bool>(\"true\"); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "does not satisfy the constraint"));
}

// ── §11.8: T.Member dispatch typing ─────────────────────────────────────────

// `T.Zero()` under `where T: INum<T>` typechecks (the constraint provides it).
BEZEL_TEST(static_iface_conformance_807, t_member_via_constraint_accepted) {
    auto r = parseSemaAll(
        std::string("module M; ") + kINum + kMeters +
        "static class F "
        "  public static method Z<T>(): T where T: INum<T> "
        "    return T.Zero(); end Z; "
        "end F;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Without ANY constraint, `T.Member` stays an error (naming §11.8).
BEZEL_TEST(static_iface_conformance_807, t_member_without_constraint_error) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method Z<T>(): int "
        "    return T.Ignore(); end Z; "
        "end F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
}

// A constraint whose interface does NOT declare the member → error.
BEZEL_TEST(static_iface_conformance_807, t_member_not_declared_error) {
    auto r = parseSemaAll(
        std::string("module M; ") + kINum +
        "static class F "
        "  public static method Z<T>(): T where T: INum<T> "
        "    return T.Nope(); end Z; "
        "end F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
}

// `T.Member` on an INSTANCE interface member → error (static members only).
BEZEL_TEST(static_iface_conformance_807, t_member_instance_member_error) {
    auto r = parseSemaAll(
        "module M; "
        "public interface IS<T> where T: IS<T> "
        "  method Scale(k: int): int; "
        "end IS; "
        "static class F "
        "  public static method Z<T>(): int where T: IS<T> "
        "    return T.Scale(2); end Z; "
        "end F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
}

// A bare `T` OUTSIDE a member-access target keeps today's unresolved error.
BEZEL_TEST(static_iface_conformance_807, bare_t_expression_still_unresolved) {
    auto r = parseSemaAll(
        std::string("module M; ") + kINum +
        "static class F "
        "  public static method Z<T>(): int where T: INum<T> "
        "    var x := T; return 0; end Z; "
        "end F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "unresolved identifier"));
}

// ── §11.8: constraint-operator typing ───────────────────────────────────────

// `a + b` on T-typed operands resolves through the constraint's operator.
BEZEL_TEST(static_iface_conformance_807, constraint_operator_accepted) {
    auto r = parseSemaAll(
        std::string("module M; ") + kINum + kMeters +
        "static class F "
        "  public static method Add2<T>(a: T, b: T): T where T: INum<T> "
        "    return a + b; end Add2; "
        "end F;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// UNCONSTRAINED `a + b` on T-typed operands stays rejected (§25).
BEZEL_TEST(static_iface_conformance_807, unconstrained_t_plus_t_rejected) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method Add2<T>(a: T, b: T): T "
        "    return a + b; end Add2; "
        "end F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An operator the constraint does NOT declare stays rejected (`-` on an
// interface declaring only `+`).
BEZEL_TEST(static_iface_conformance_807, undeclared_operator_rejected) {
    auto r = parseSemaAll(
        std::string("module M; ") + kINum + kMeters +
        "static class F "
        "  public static method Sub2<T>(a: T, b: T): T where T: INum<T> "
        "    return a - b; end Sub2; "
        "end F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
