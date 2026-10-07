// fix #5473 / spec §14.2 / §5.1 / §22.3 (v0.315): a bare type parameter's
// member surface reaches an INHERITED generic-interface member, so the
// constraint-receiver substitution must walk the bound interface's base chain
// — not only match the bound's own declaration.
//
// THE GAP. `constraintReceiverSubst` (src/sema/ExprTypecheck_Calls.cpp), added
// by #5469, builds the member-parameter-name -> constraint-argument map by
// requiring `bound->declaration == memberParent` — the member's OWNING type. That
// holds for a member declared directly on the bound interface:
//
//   interface IEquatable<E> { method Equals(other: E): bool; }
//   where T: IEquatable<Other> => a.Equals(u)
//
// It does NOT hold for a member INHERITED from that interface's base:
//
//   interface IBase<E> { method Take(x: E): bool; }
//   interface IDer<E> : IBase<E>;
//   where T: IDer<Other> => a.Take(u)
//
// Here `memberParent` is `IBase` and the bound's declaration is `IDer`. The
// direct match never fired, `substituteMemberType`'s own base-chain walk cannot
// reach it either (the receiver is a bare `T` with no declaration and no base
// chain — there is no class on this path at all), so `Take`'s slot kept `E`
// unsubstituted. `checkAssignable` cannot check a bare declaration-less type
// parameter against a concrete class, so the slot was NOT CHECKED AT ALL and the
// wrong argument was silently admitted.
//
// MEASURED on `r23-c28` (the tree this fix applies to), for the reject row
// `unsubstituted_receiver_inherited_member_rejects_wrong_argument` below: the
// program COMPILES and LINKS, and then the WRONG ARGUMENT is executed —
// `runAot` reports `terminated by signal 11` (SIGSEGV), and under
// `scripts/agent_check.sh --probe --asan` the same program reports
//
//   ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 8
//     #0 bz$M.Other$Len
//     #1 bz$M.CD$Take
//     #2 main
//   0 bytes after the 16-byte `Unrelated` allocation
//
// — byte-identical in signature to the overflow that sank a REJECTED earlier
// narrowing of #5469. That is what makes this a soundness row and not a
// tidiness one: the wrong argument reaches a body that reads through the
// `Other`-typed parameter.
//
// A NOTE ON WHY THE NEEDLE IS LOAD-BEARING IN THAT FIRST ROW. Pre-fix the
// program is not silently wrong-and-running: it CRASHES, and `runAot` surfaces
// the crash as a `Severity::Error` diagnostic. So `hasErrors()` and `!ran`
// hold pre-fix too, and a row asserting only those two would PASS on the broken
// compiler — the vacuous-oracle trap. `hasTargetTypeConvertError` is what makes
// it honest: it asserts the REJECTION carries the §5.1 argument diagnostic,
// which is what tells a correct compile-time rejection apart from a program
// that merely died. Every reject row below therefore asserts a needle, never
// `hasErrors()` alone.
//
// THE FIX. `ancestorGenericSubst` — the base-chain walk `substituteMemberType`
// already ran for a CONCRETE receiver — is now shared with
// `constraintReceiverSubst`, which passes `throughInterfaces=true` (a bare type
// parameter's surface is established by its constraint interface and that
// interface's ancestors, per §14.2's member-surface clause). The concrete-
// receiver call passes `false` and reproduces the previous class-only walk
// exactly, so no program that compiled through that arm compiles differently
// now.
//
// THE SHAPES NAMED BY THE ISSUE, AND WHAT THIS FILE PINS. The issue lists three
// residual shapes. Two of them are NOT open on `r23-c28`, and this file pins
// that measurement rather than leaving it to a re-derivation:
//   (a) DIFFERING SPELLING (`interface IEquatable<E>` with `where T:
//       IEquatable<T>`) — MEASURED REJECTED pre-fix. The substitution keys on
//       the bound's DECLARATION, not on the name, so a differing spelling is
//       already covered; pinned by
//       `differing_spelling_self_target_rejects_wrong_argument`.
//   (c) TRANSITIVE where-CHAIN (`where T: Q`, `where Q: IEquatable<Other>`) —
//       MEASURED REJECTED pre-fix. `forEachConstraintBound` follows `where T: U`
//       chains (ExprTypecheck.cpp:11656), so the bound is reached either way;
//       pinned by `transitive_where_chain_rejects_wrong_argument`.
//   (b) BASE-INTERFACE CHAIN — the open one, and the reason for this file.
//
// SPEC BASIS. §14.2: "`where T: IFoo` admits exactly the substitutions for
// which T conforms to `IFoo` … Inside the generic body, T's surface includes
// IFoo's members, regardless of which conformance category T belongs to." An
// INHERITED member is part of that surface — §11.2 states an interface that
// extends another exposes the extended interface's members too — so the surface
// `T` exposes is `IBase`'s members AS INSTANTIATED at `IDer<Other>`
// (i.e. `Take(x: Other)`), which is what makes the reject rows sound.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The §5.1 call-argument rejection (`checkAssignable`, "argument `x` to call to
// `M` …") whose wording names the TARGET TYPE. Asserting that form is what
// proves the argument was checked against the REAL substituted type rather than
// against a bare type parameter (whose rejection reads "not implicitly
// convertible to the type parameter `T`" instead).
bool hasTargetTypeConvertError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible to the target type") !=
                std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// The §22.3 referent-type rejection for a `ref`/`out` argument. A separate
// needle from the §5.1 one because the `ref`/`out` arm runs its own check with
// its own wording — a fix reaching only the value-argument path would leave
// this one open, so the two are pinned independently.
bool hasRefOutReferentError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not match the parameter's referent type") !=
                std::string::npos &&
            d.message.find("22.3") != std::string::npos)
            return true;
    return false;
}

// Shared library half: an `Other` (the constraint's argument) and an
// `Unrelated` (a class with no relation to it — the argument that must be
// rejected). `Other.Len()` exists so a body can actually READ through the
// parameter: without a read the wrong-argument program is merely ill-typed;
// with one it is the type confusion measured under ASan.
const std::string kPrelude =
    "module M; "
    "public class Other "
    "  public S: string; "
    "  public constructor(s: string) S := s; end constructor; "
    "  public method Len(): int return S.Length; end Len; "
    "end Other; "
    "public class Other2 "
    "  public S: string; "
    "  public constructor(s: string) S := s; end constructor; "
    "  public method Len(): int return S.Length; end Len; "
    "end Other2; "
    "public class Unrelated "
    "  public Z: int; "
    "  public constructor() Z := 1; end constructor; "
    "end Unrelated; ";

bezel::test::JitResult runProgram(const std::string& decls,
                                  const std::string& mainBody) {
    return runAot(kPrelude + decls +
                  "public class Program "
                  "  public static method Main(): int " + mainBody +
                  "  end Main; "
                  "end Program;");
}

}  // namespace

// ============================================================
// (b) THE OPEN SHAPE — an INHERITED member on the bound's base
// interface. These are the rows that fail without the fix; the ASan
// overflow is measured on the first of them.
// ============================================================

// §5.1 / §14.2 / §11.2 (#5473): THE REJECTION ROW. `Take` is declared on
// `IBase<E>`, not on the bound `IDer<E>`, so `IBase` is the member's owning type
// and the constraint-receiver substitution must reach it through `IDer`'s base
// chain. Composed, `IDer<Other>` fixes `IBase` at `IBase<Other>`, so `Take`
// demands an `Other` and `Unrelated` has no subtype relation to it.
//
// `CD.Take`'s body READS `x.Len()` — the exact program measured overflowing the
// heap under ASan. REJECTION is the soundness assertion here, not a control:
// without the fix this row compiles, runs and overflows.
BEZEL_TEST(constraint_receiver_basechain_5473,
           unsubstituted_receiver_inherited_member_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Take(x: E): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Take(x: Other): bool "
        "    return x.Len() = 2; "
        "  end Take; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IDer<Other> => a.Take(u); "
        "end H; ",
        "using var c := new CD(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5473): the accept control for the row above — same declared
// chain, the MATCHING argument. `IDer<Other>` fixes `IBase<Other>`, so
// `Take(x: Other)` and the argument is an `Other`: a §5.1 identity conversion.
// This is the direction a substitution that over-reached could break, and it is
// what proves the reject row above is a type error rather than a blanket
// refusal of inherited members.
BEZEL_TEST(constraint_receiver_basechain_5473,
           unsubstituted_receiver_inherited_member_accepts_matching_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Take(x: E): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Take(x: Other): bool "
        "    return x.Len() = 2; "
        "  end Take; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, o: Other): bool "
        "    where T: IDer<Other> => a.Take(o); "
        "end H; ",
        "using var c := new CD(); "
        "Console.WriteLine(H.F(c, new Other(\"ab\"))); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §14.2 (#5473): a TWO-LEVEL chain (`IBase <- IMid <- IDer`). The walk
// composes each level's arguments, so `Take` still demands the `Other` the
// bound fixes two hops up. Pinned separately because a walk that stopped at the
// immediate base would reject the two-level ACCEPT control below while leaving
// this row unchecked.
BEZEL_TEST(constraint_receiver_basechain_5473,
           two_level_base_chain_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Take(x: E): bool; "
        "end IBase; "
        "public interface IMid<E>: IBase<E> "
        "end IMid; "
        "public interface IDer<E>: IMid<E> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Take(x: Other): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IDer<Other> => a.Take(u); "
        "end H; ",
        "using var c := new CD(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5473): the two-level ACCEPT control, sharing the declarations
// with the reject row above and differing only in the argument's type.
BEZEL_TEST(constraint_receiver_basechain_5473,
           two_level_base_chain_accepts_matching_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Take(x: E): bool; "
        "end IBase; "
        "public interface IMid<E>: IBase<E> "
        "end IMid; "
        "public interface IDer<E>: IMid<E> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Take(x: Other): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, o: Other): bool "
        "    where T: IDer<Other> => a.Take(o); "
        "end H; ",
        "using var c := new CD(); "
        "Console.WriteLine(H.F(c, new Other(\"ab\"))); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §14.2 (#5473): DIFFERING SPELLING on the INHERITED member. `IBase`'s
// parameter is spelled `Elem`, the bound `IDer`'s is `E`, and the constraint's
// argument is `Other` — three unrelated declarations. Pinned because the walk
// must compose by DECLARATION position, and a name-keyed implementation would
// confuse the two. This is the same collision class the #5469 rows pin on the
// direct-declaration shape.
BEZEL_TEST(constraint_receiver_basechain_5473,
           inherited_member_differing_spelling_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<Elem> "
        "  method Take(x: Elem): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Take(x: Other): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IDer<Other> => a.Take(u); "
        "end H; ",
        "using var c := new CD(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5473): TWO base parameters, fixed to DIFFERENT concrete types
// (`IDer<E>: IBase<E, Other2>`). Composition is positional, so `Take` demands
// `Other` in the first slot and `Other2` in the second; the first slot's wrong
// argument is rejected. Pinned because a fix that substituted only the first
// position — or that resolved each position by name — would leave this open.
BEZEL_TEST(constraint_receiver_basechain_5473,
           inherited_member_two_params_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<A, B> "
        "  method Take(x: A, y: B): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E, Other2> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Take(x: Other, y: Other2): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IDer<Other> => a.Take(u, new Other2(\"q\")); "
        "end H; ",
        "using var c := new CD(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5473): the two-parameter ACCEPT control — both slots get their
// composed types, so both arguments match.
BEZEL_TEST(constraint_receiver_basechain_5473,
           inherited_member_two_params_accepts_matching_arguments_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<A, B> "
        "  method Take(x: A, y: B): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E, Other2> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Take(x: Other, y: Other2): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, o: Other): bool "
        "    where T: IDer<Other> => a.Take(o, new Other2(\"q\")); "
        "end H; ",
        "using var c := new CD(); "
        "Console.WriteLine(H.F(c, new Other(\"ab\"))); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 / §28.1 (#5473): an OWNERSHIP-TRANSFER position on the inherited member.
// `Move(owned x: E)` under `T: IDer<Other>` — the composed target is the
// concrete `Other`, so an `owned Unrelated` is rejected before the transfer can
// be typed against a mismatched layout. Pinned separately (its own needle too)
// because the `ref`/`out`/`owned` transfer path is a DIFFERENT arm of
// `checkArgsAgainstParams` from the value path.
BEZEL_TEST(constraint_receiver_basechain_5473,
           inherited_owned_parameter_rejects_wrong_argument_spec_22_3) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Move(owned x: E): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Move(owned x: Other): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IDer<Other> => a.Move(owned u); "
        "end H; ",
        "using var c := new CD(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §22.3 (#5473): the ownership-transfer ACCEPT control. The composed target is
// `Other` and the transferred value is an `Other`, so the call is admitted; the
// callee owns the value for the rest of its scope, so the call site does not
// `using`-bind it.
BEZEL_TEST(constraint_receiver_basechain_5473,
           inherited_owned_parameter_accepts_matching_argument_spec_22_3) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Move(owned x: E): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Move(owned x: Other): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T): bool "
        "    where T: IDer<Other> => a.Move(owned new Other(\"ab\")); "
        "end H; ",
        "using var c := new CD(); "
        "Console.WriteLine(H.F(c)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 (#5473): a `ref` (ALIASING) position on the inherited member. §22.3
// admits no implicit conversion for a `ref`/`out` argument, and the arm that
// enforces it is separate from the value-argument one — the fix reaches it
// through the same `constraintReceiverSubst` call at the `ref`/`out` site, and
// this row is what proves that call site was reached.
BEZEL_TEST(constraint_receiver_basechain_5473,
           inherited_ref_parameter_rejects_wrong_argument_spec_22_3) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Bump(ref x: E): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E> "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Bump(ref x: Other): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IDer<Other> => a.Bump(ref u); "
        "end H; ",
        "using var c := new CD(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRefOutReferentError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5473): the SELF-TARGET bound through a base chain —
// `where T: IDer<T>` composes to `IBase<T>`, leaving the receiver's own
// (still bare) `T` as the target. `Unrelated` is then rejected by §5.1's
// "not convertible to a bare type parameter" arm, and the message names the
// type parameter rather than a target type. This is the row that DISCRIMINATES
// a real composition from the pre-fix no-op: a fix that simply refused the
// inherited member outright would also reject it, but would then fail the
// accept controls above.
BEZEL_TEST(constraint_receiver_basechain_5473,
           inherited_member_self_target_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Take(x: E): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E> "
        "end IDer; "
        "public class CS: IDer<CS> "
        "  public constructor() end constructor; "
        "  public method Take(x: CS): bool => true; "
        "end CS; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IDer<T> => a.Take(u); "
        "end H; ",
        "using var c := new CS(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5473): the self-target ACCEPT control through the base chain —
// the composed `IBase<T>` is the identity here, so `a.Take(o)` with `o: T` is
// admitted. Control against the reject row above: same bound, same chain,
// differing only in the argument's type.
BEZEL_TEST(constraint_receiver_basechain_5473,
           inherited_member_self_target_accepts_matching_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IBase<E> "
        "  method Take(x: E): bool; "
        "end IBase; "
        "public interface IDer<E>: IBase<E> "
        "end IDer; "
        "public class CS: IDer<CS> "
        "  public constructor() end constructor; "
        "  public method Take(x: CS): bool => true; "
        "end CS; "
        "public static class H "
        "  public static method F<T>(a: T, o: T): bool "
        "    where T: IDer<T> => a.Take(o); "
        "end H; ",
        "using var c := new CS(); "
        "Console.WriteLine(H.F(c, new CS())); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 (#5473): a NON-GENERIC base interface. `IBase` declares no type
// parameter, so the walk reaches it but composes nothing, and `memberParent`'s
// `genericParams` is empty — the guard at the top of
// `constraintReceiverSubst` returns an EMPTY map without walking. The member's
// signature is already concrete (`Take(x: Other)`), so the ordinary §5.1 check
// runs and rejects the wrong argument unchanged. Control that the fix does not
// disturb a base that contributes no arguments, and that the rejection comes
// from the pre-existing check rather than from the new walk.
BEZEL_TEST(constraint_receiver_basechain_5473,
           non_generic_base_interface_unchanged_spec_14_2) {
    auto r = runProgram(
        "public interface IPlain "
        "  method Take(x: Other): bool; "
        "end IPlain; "
        "public interface IDer<E>: IPlain "
        "end IDer; "
        "public class CD: IDer<Other> "
        "  public constructor() end constructor; "
        "  public method Take(x: Other): bool => true; "
        "end CD; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IDer<Other> => a.Take(u); "
        "end H; ",
        "using var c := new CD(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// THE ISSUE'S OTHER TWO SHAPES — MEASURED already rejected on
// `r23-c28`. Pinned so the measurement is not re-derived from a
// hypothesis, and so a future refactor that regresses either is
// caught.
// ============================================================

// §5.1 / §14.2 (#5473): shape (a), DIFFERING SPELLING, direct member. The
// interface's parameter is `E`, the constraint's own argument is the receiver's
// `T` (`where T: IEquatable<T>`), and they are unrelated declarations that
// happen to be reached through different names. MEASURED REJECTED on `r23-c28`
// before this fix — the substitution keys on the bound's DECLARATION, so a
// differing spelling never opened a hole. Pinned for that reason.
BEZEL_TEST(constraint_receiver_basechain_5473,
           differing_spelling_self_target_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IEquatable<E> "
        "  method Equals(other: E): bool; "
        "end IEquatable; "
        "public class C2: IEquatable<Other> "
        "  public constructor() end constructor; "
        "  public method Equals(other: Other): bool "
        "    return other.Len() = 2; "
        "  end Equals; "
        "end C2; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IEquatable<T> => a.Equals(u); "
        "end H; ",
        "using var c := new C2(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5473): shape (c), TRANSITIVE where-CHAIN. The receiver's own
// clause names `Q`, not the interface, and the interface's obligation lives on
// `Q`'s clause. MEASURED REJECTED on `r23-c28` before this fix —
// `forEachConstraintBound` follows `where T: U` chains (ExprTypecheck.cpp), so
// the bound is reached and the direct-declaration substitution fires. Pinned so
// the chain-following behaviour stays covered by a row of its own.
BEZEL_TEST(constraint_receiver_basechain_5473,
           transitive_where_chain_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IEquatable<E> "
        "  method Equals(other: E): bool; "
        "end IEquatable; "
        "public class C2: IEquatable<Other> "
        "  public constructor() end constructor; "
        "  public method Equals(other: Other): bool "
        "    return other.Len() = 2; "
        "  end Equals; "
        "end C2; "
        "public static class H "
        "  public static method F<T, Q>(a: T, u: Unrelated): bool "
        "    where T: Q "
        "    where Q: IEquatable<Other> => a.Equals(u); "
        "end H; ",
        "using var c := new C2(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT CONTROLS — shapes the walk must NOT change.
// ============================================================

// §14.1 / §14.2 (#5473): the CONCRETE-receiver base-chain walk, which the fix
// reuses rather than reimplements. `IntHolder: Base<int>` calling Base's
// `GetVal(): T` — #722's shape — must still substitute `T` to `int`, so the
// inherited member's result type is a concrete `int` and `* 2` resolves (§25).
// Control that the shared helper reproduces the arm it replaced
// (`throughInterfaces=false`, class-only) for a receiver that is not a bare
// type parameter. The receiver is spelled `this.GetVal()` because the
// implicit-this spelling is a SEPARATE pre-existing gap (it types as `?` with or
// without this fix — MEASURED on both trees — and is not this issue's shape).
BEZEL_TEST(constraint_receiver_basechain_5473,
           concrete_receiver_base_chain_still_substitutes_spec_14_1) {
    auto r = runProgram(
        "public class Base<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method GetVal(): T => _v; "
        "end Base; "
        "public class IntHolder: Base<int> "
        "  public constructor() base(7); end constructor; "
        "  public method Doubled(): int return this.GetVal() * 2; end Doubled; "
        "end IntHolder; ",
        "using var h := new IntHolder(); "
        "Console.WriteLine(h.Doubled()); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 (#5473): an UNCONSTRAINED generic — no `where`-clause at all. The
// receiver has no constraint bound, so the walk is never entered and the
// ordinary unconstrained-generic path runs unchanged.
BEZEL_TEST(constraint_receiver_basechain_5473,
           unconstrained_generic_receiver_unchanged_spec_14_2) {
    auto r = runProgram(
        "public class Plain "
        "  public W: int; "
        "  public constructor() W := 3; end constructor; "
        "end Plain; "
        "public static class NH "
        "  public static method F<T>(a: T): T return a; end F; "
        "end NH; ",
        "using var p := new Plain(); "
        "Console.WriteLine(NH.F(p).W); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 (#5473): an UNRELATED bound — `where T: IPlain`, a non-generic
// interface that declares no `Take`. The walk starts at `IPlain`, finds no
// ancestor named `IBase`, and returns false, leaving the map EMPTY; the
// member-access site's own diagnostic (§3.6 rule 2 / §14.2) is what reports the
// call. Control that a failed walk stays the conservative no-op rather than
// inventing a binding.
BEZEL_TEST(constraint_receiver_basechain_5473,
           unrelated_bound_leaves_surface_unchanged_spec_14_2) {
    auto r = runProgram(
        "public interface IPlain "
        "  method Note(): bool; "
        "end IPlain; "
        "public class CP: IPlain "
        "  public constructor() end constructor; "
        "  public method Note(): bool => true; "
        "end CP; "
        "public static class H "
        "  public static method F<T>(a: T): bool "
        "    where T: IPlain => a.Take(1); "
        "end H; ",
        "using var c := new CP(); "
        "Console.WriteLine(H.F(c)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}