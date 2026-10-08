// fix-forward #5469 / spec §14.2 / §5.1 (v0.315): a member call on a
// receiver that is ITSELF an unsubstituted type parameter checks its
// arguments against the CONSTRAINT'S OWN instantiation of the member's
// signature, not against the interface's literal type parameter.
//
// THE SHAPE. `F<T>(a: T, o: Other) where T: IEquatable<Other> =>
// a.Equals(o)`. The receiver `a` is a bare `Kind::Named` `T` — no
// declaration, no generic arguments — so `substituteMemberType`'s "Direct"
// arm (guarded on `recvTy->declaration == parent &&
// !recvTy->genericArgs.empty()`) cannot fire and its `derived` walk finds no
// base chain. The interface's OWN `T` therefore survived unsubstituted in
// `Equals(other: T)`, and #5469's `checkAssignable` arm matched that bare
// name against the ENCLOSING method's `T` — a DIFFERENT declaration that
// merely shares the spelling.
//
// WHY THIS FILE EXISTS. The previously-attempted fix (widening
// `classParent`'s params into `calleeFreeParamNames`) shipped green because
// NO row covered the direction that matters for memory safety: an
// unsubstituted receiver with a WRONG argument. Every row in this file that
// pins REJECTION is that missing quadrant, and the `p10` row below is the
// #4863 bound with a body that actually READS the parameter's field — the
// exact program §17 review ran under ASan.
//
// MEASURED, on the tree this fix applies to (`1917f11`, an ancestor of
// staging): the accept controls REJECT, so the reject rows here are the ones
// that would have regressed to a silent accept — and #17 review measured the
// narrowing accepting them, compiling, linking, RUNNING, and ASan reporting
//   ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 8
//   #0 bz$M.Other$Len #1 bz$M.C2$Equals #2 main
// reading 0 bytes after the 16-byte `Unrelated` allocation. A REJECTION row
// is therefore the load-bearing artifact here, not a control.
//
// THE FIX. `ExprTC::constraintReceiverSubst`
// (src/sema/ExprTypecheck_Calls.cpp) builds the interface-param-name ->
// constraint-arg map from the receiver's own `where`-clause and applies it
// to the parameter type before `checkAssignable`, so the target type is the
// real substituted type. Both directions then fall out of one ordinary §5.1
// comparison: the matching argument is an identity conversion, the
// non-conforming one has no subtype relation to the concrete target.
//
// SPEC BASIS. §14.2: "`where T: IFoo` admits exactly the substitutions for
// which T conforms to `IFoo` … Inside the generic body, T's surface includes
// IFoo's members, regardless of which conformance category T belongs to; the
// body cannot tell whether T was satisfied by a class, a record, an enum, an
// interface, a primitive, or another type parameter, and does not need to."
// The surface `T` exposes is `IFoo`'s members AS INSTANTIATED at
// `IFoo<Other>` — which is what makes the reject rows below sound: `T` at
// `IEquatable<Other>` really does require an `Other`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The §5.1 call-argument rejection (`checkAssignable`, "argument `x` to call
// to `M` …") whose wording names the TARGET TYPE. This is the diagnostic the
// substitution makes reachable: pre-fix the target was the interface's own
// bare `T`, so the message named a type parameter ("not implicitly
// convertible to the type parameter `T`"); post-fix the target is the
// constraint's concrete argument and the ordinary "not implicitly convertible
// to the target type" form fires. Asserting that form is what proves the
// argument was checked against the REAL substituted type and not against a
// spelling.
bool hasTargetTypeConvertError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible to the target type") !=
                std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// Shared library half: an `Other` (the constraint's argument) and an
// `Unrelated` (a class with no relation to it — the argument that must be
// rejected). `Other.Len()` exists so a body can actually READ through the
// parameter: without a read the wrong-argument program is merely ill-typed,
// with one it is the type confusion §17 review measured under ASan.
const std::string kPrelude =
    "module M; "
    "public class Other "
    "  public S: string; "
    "  public constructor(s: string) S := s; end constructor; "
    "  public method Len(): int return S.Length; end Len; "
    "end Other; "
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
// THE MISSING QUADRANT — unsubstituted receiver, WRONG argument.
// These are the rows the rejected fix would have failed.
// ============================================================

// §5.1 / §14.2 (#5469): `where T: IEquatable<Other>` makes
// `Equals(other: Other)`; `Unrelated` has no subtype relation to `Other`, so
// the call is rejected. The body READS `other.Len()` — the exact program
// that overflowed the heap under the narrowing fix. REJECTION is the
// soundness assertion, not a control.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           unsubstituted_receiver_other_bound_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public class C2: IEquatable<Other> "
        "  public constructor() end constructor; "
        "  public method Equals(other: Other): bool "
        "    return other.Len() = 2; "
        "  end Equals; "
        "end C2; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IEquatable<Other> => a.Equals(u); "
        "end H; ",
        "using var c := new C2(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5469): the SELF-target bound `where T: IEquatable<T>`.
// Substituting the constraint's argument leaves the parameter as the
// ENCLOSING method's `T`, which is still a bare type parameter — so
// `Unrelated` is rejected by #5469's own §5.1 arm, and the message names the
// type parameter. This is the row that discriminates a real substitution
// (which cannot resolve a self-referential bound) from the rejected
// narrowing's free-set widening (which would have accepted it).
BEZEL_TEST(constraint_receiver_sigsub_5469,
           unsubstituted_receiver_self_target_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public class CS: IEquatable<CS> "
        "  public constructor() end constructor; "
        "  public method Equals(other: CS): bool => true; "
        "end CS; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IEquatable<T> => a.Equals(u); "
        "end H; ",
        "using var c := new CS(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.2 / §22.3 / §28.1 (#5469): an OWNERSHIP-TRANSFER position. `Move(owned
// x: Other)` under `T: IMove<Other>` — the substituted target is the concrete
// `Other`, so `owned u` of an `Unrelated` is rejected before the transfer can
// be typed against a mismatched layout. Pinned separately because a fix that
// only reached the value-argument path would leave this one open.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           unsubstituted_receiver_owned_parameter_rejects_wrong_argument_spec_22_3) {
    auto r = runProgram(
        "public interface IMove<T> "
        "  method Move(owned x: T): bool; "
        "end IMove; "
        "public class CM: IMove<Other> "
        "  public constructor() end constructor; "
        "  public method Move(owned x: Other): bool => true; "
        "end CM; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IMove<Other> => a.Move(move u); "
        "end H; ",
        "using var c := new CM(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.2 (#5469): DIFFERENT NESTING — a class-level `T` coexists with a
// method-level `Q`, and the constraint is on `Q` (`Q: IMatch<Other>`) while
// the interface's own parameter is also spelled `T`. The substitution must key
// on the RECEIVER's parameter (`Q`) and the member's OWNING interface
// (`IMatch`), so the class-level `T` is irrelevant; the wrong argument is
// rejected. Pinned because a name-only implementation would confuse the two
// `T`s — the same spelling collision that caused the original defect.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           unsubstituted_receiver_differing_nesting_rejects_wrong_argument_spec_14_2) {
    auto r = runProgram(
        "public interface IMatch<T> "
        "  method Matches(other: T): bool; "
        "end IMatch; "
        "public class CMM: IMatch<Other> "
        "  public constructor() end constructor; "
        "  public method Matches(other: Other): bool => true; "
        "end CMM; "
        "public static class Holder<T> "
        "  public static method F<Q>(a: Q, u: Unrelated): bool "
        "    where Q: IMatch<Other> => a.Matches(u); "
        "end Holder; ",
        "using var c := new CMM(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(Holder<Other>.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5469): TWO interface parameters, both bound to `Other`, both
// filled with the wrong argument. Each is rejected on its own — a fix that
// substituted only the first position would leave the second unchecked.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           unsubstituted_receiver_two_params_reject_wrong_arguments_spec_5_1) {
    auto r = runProgram(
        "public interface IZip<T, Q> "
        "  method Zip(x: T, y: Q): bool; "
        "end IZip; "
        "public class CZ: IZip<Other, Other> "
        "  public constructor() end constructor; "
        "  public method Zip(x: Other, y: Other): bool => true; "
        "end CZ; "
        "public static class H "
        "  public static method F<T, Q>(a: T, u: Unrelated): bool "
        "    where T: IZip<Other, Other> => a.Zip(u, u); "
        "end H; ",
        "using var c := new CZ(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §5.1 / §14.2 (#5469): the interface's parameter is spelled DIFFERENTLY from
// the receiver's (`IVal<E>` reached through `T: IVal<Other>`). Here the
// spelling collision that caused the original defect does not arise, so this
// shape was ALREADY accepted pre-fix — but its wrong-argument twin was
// silently accepted too, reaching codegen and degrading the result to a
// zero/null return with an `internal:` warning. Both directions are pinned so
// the substituted target type is the real `Other` rather than the interface's
// `E`.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           differing_interface_param_name_rejects_wrong_argument_spec_5_1) {
    auto r = runProgram(
        "public interface IVal<E> "
        "  method Val(x: E): bool; "
        "end IVal; "
        "public class CV: IVal<Other> "
        "  public constructor() end constructor; "
        "  public method Val(x: Other): bool => true; "
        "end CV; "
        "public static class H "
        "  public static method F<T>(a: T, u: Unrelated): bool "
        "    where T: IVal<Other> => a.Val(u); "
        "end H; ",
        "using var c := new CV(); "
        "using var u := new Unrelated(); "
        "Console.WriteLine(H.F(c, u)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTargetTypeConvertError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT CONTROLS — the direction the over-rejection broke.
// ============================================================

// §5.1 / §14.2 (#5469): the matching argument. `where T: IEquatable<Other>`
// makes the substituted target `Other`, so `a.Equals(o: Other)` is a §5.1
// identity conversion. This is the shape that FAILED pre-fix and is the
// #4863 accept control re-derived against the substitution reading.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           unsubstituted_receiver_other_bound_accepts_matching_argument_spec_5_1) {
    auto r = runProgram(
        "public class C2: IEquatable<Other> "
        "  public constructor() end constructor; "
        "  public method Equals(other: Other): bool "
        "    return other.Len() = 2; "
        "  end Equals; "
        "end C2; "
        "public static class H "
        "  public static method F<T>(a: T, o: Other): bool "
        "    where T: IEquatable<Other> => a.Equals(o); "
        "end H; ",
        "using var c := new C2(); "
        "Console.WriteLine(H.F(c, new Other(\"ab\"))); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §14.2 (#5469): the SELF-target bound with the matching argument —
// `where T: IEquatable<T>`, `a.Equals(o)` with `o: T`. The substitution is
// the identity here, so both sides stay the enclosing `T` and the call is
// admitted. Control against the reject row above, which shares the bound and
// differs only in the argument's type.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           unsubstituted_receiver_self_target_accepts_matching_argument_spec_14_2) {
    auto r = runProgram(
        "public class CS: IEquatable<CS> "
        "  public constructor() end constructor; "
        "  public method Equals(other: CS): bool => true; "
        "end CS; "
        "public static class H "
        "  public static method F<T>(a: T, o: T): bool "
        "    where T: IEquatable<T> => a.Equals(o); "
        "end H; ",
        "using var c := new CS(); "
        "Console.WriteLine(H.F(c, new CS())); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 / §28.1 (#5469): the OWNERSHIP-TRANSFER accept control. `a.Move(owned
// x: Other)` under `T: IMove<Other>` — the substituted target is the concrete
// `Other` and the transferred value is an `Other`, so the call is admitted.
// The value is NOT `using`-bound at the call site: the transfer moves it into
// the callee, which owns it for the rest of its scope.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           unsubstituted_receiver_owned_parameter_accepts_matching_argument_spec_22_3) {
    auto r = runProgram(
        "public interface IMove<T> "
        "  method Move(owned x: T): bool; "
        "end IMove; "
        "public class CM: IMove<Other> "
        "  public constructor() end constructor; "
        "  public method Move(owned x: Other): bool => true; "
        "end CM; "
        "public static class H "
        "  public static method F<T>(a: T): bool "
        "    where T: IMove<Other> => a.Move(owned new Other(\"ab\")); "
        "end H; ",
        "using var c := new CM(); "
        "Console.WriteLine(H.F(c)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 (#5469): the DIFFERENT-NESTING accept control, sharing the bound and
// the class-level `T` with the reject row above.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           differing_nesting_accepts_matching_argument_spec_14_2) {
    auto r = runProgram(
        "public interface IMatch<T> "
        "  method Matches(other: T): bool; "
        "end IMatch; "
        "public class CMM: IMatch<Other> "
        "  public constructor() end constructor; "
        "  public method Matches(other: Other): bool => true; "
        "end CMM; "
        "public static class Holder<T> "
        "  public static method F<Q>(a: Q, o: Other): bool "
        "    where Q: IMatch<Other> => a.Matches(o); "
        "end Holder; ",
        "using var c := new CMM(); "
        "Console.WriteLine(Holder<Other>.F(c, new Other(\"ab\"))); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §14.2 (#5469): the TWO-PARAMETER accept control, both arguments of
// the constraint's own type.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           two_params_accept_matching_arguments_spec_5_1) {
    auto r = runProgram(
        "public interface IZip<T, Q> "
        "  method Zip(x: T, y: Q): bool; "
        "end IZip; "
        "public class CZ: IZip<Other, Other> "
        "  public constructor() end constructor; "
        "  public method Zip(x: Other, y: Other): bool => true; "
        "end CZ; "
        "public static class H "
        "  public static method F<T, Q>(a: T, o: Other): bool "
        "    where T: IZip<Other, Other> => a.Zip(o, o); "
        "end H; ",
        "using var c := new CZ(); "
        "Console.WriteLine(H.F(c, new Other(\"ab\"))); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 (#5469): a constraint whose bound is a CLASS, not a generic interface
// — `where T: Cell`, calling a `Take(v: Cell)` member on `a`. The bound's
// TypeRef has no type arguments, so the substitution map is empty and the
// member's signature (already concrete) is checked unchanged. Control that
// the fix does not disturb non-generic bounds.
BEZEL_TEST(constraint_receiver_sigsub_5469,
           class_bound_receiver_unchanged_spec_14_2) {
    auto r = runProgram(
        "public class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Cell; "
        "public interface ITake "
        "  method Take(v: Cell): bool; "
        "end ITake; "
        "public class CT: ITake "
        "  public constructor() end constructor; "
        "  public method Take(v: Cell): bool => true; "
        "end CT; "
        "public static class H "
        "  public static method F<T>(a: T, c: Cell): bool "
        "    where T: ITake => a.Take(c); "
        "end H; ",
        "using var c := new CT(); "
        "using var cell := new Cell(7); "
        "Console.WriteLine(H.F(c, cell)); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 (#5469): an UNCONSTRAINED generic — `NH.F<T>(a: T): T` with no
// `where`-clause. The receiver has no constraint bound at all, so the
// substitution map is empty and the ordinary unconstrained-generic path runs
// unchanged. Control that the fix only activates where a constraint exists.
BEZEL_TEST(constraint_receiver_sigsub_5469,
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