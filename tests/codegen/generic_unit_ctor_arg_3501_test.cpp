// narrow #3501 — a unit-typed argument at a CLASS-generic constructor formal
// monomorphized to `()` (spec §4.7 *The unit type*, §7.10 *Generics and
// nullability*, §14.1 *Generic declarations*, §22.7 *Constructors* — v0.283).
//
// ── WHY IT MUST COMPILE ─────────────────────────────────────────────────────
//
// §7.10, verbatim (v0.283):
//
//   "`where T: value` restricts T to non-nullable value types — records,
//    enums, choices ([§35]), primitives, inline arrays (all forms, including
//    `[N owned]…` owning forms), and the unit type."
//
// So `B<()>` is a legal instantiation. §4.7 — "It is the return type of methods
// declared without an explicit return type" — makes `C.Nothing()` a value of
// that type, and §4.7's "Wrapping the unit type with `?` (yielding `()?`) is
// permitted by the grammar and produces an optional with two values, `null` and
// `()`" makes `T?` at `T = ()` a slot with two distinguishable values.
//
// ── WHAT WAS MEASURED, ON ef339fa (this file's parent) ──────────────────────
//
//     new B<()>(C.Nothing())            ->  :8:30: internal: lowerExpr returned
//                                            null for call argument in
//                                            buildOrderedArgs — #1386   (x2)
//     class D: B<()> … base(C.Nothing())->  :7:10: internal: lowerExpr returned
//                                            null for base-constructor
//                                            argument — #1386
//
// exit 1 for both. The NON-generic spelling of each compiled and ran correctly
// after #3433, so the axis is specifically "the formal's declared type is a
// CLASS-level type parameter monomorphized to the unit type".
//
// ── WHAT THE BUG WAS ────────────────────────────────────────────────────────
//
// The §4.7 rescue (`unitValueForTarget` / `unitValueForCtorParam`) asks what
// type the FORMAL has, and answers through `resolveTypeParamRef`, which reads
// `genericSubstSemaTy_`. Both argument loops run with that binding ABSENT:
//
//   * `lowerNew` lowers constructor arguments under the ENCLOSING scope's
//     substitution on purpose (#597 — they are enclosing-scope expressions),
//     so the constructed class's own `T -> ()` was swapped out first.
//   * `emitChainCall` builds the base instantiation's substitution only AFTER
//     its argument loop has run (the #617 block).
//
// The formal therefore stayed a bare `Named T`, neither the `Kind::Unit` nor
// the `isNullableUnitTy` test matched, and the #1386 ICE fired. The fix carries
// the callee's UNIT bindings — only those, and only onto names the enclosing
// substitution does not already bind — across the argument loop, so the
// existing resolver answers instead of a second one being written
// (`carryCalleeUnitSubst`, LlvmBackend_impl.hpp). The chain site gets its map
// from `computeAncestorGenericSubst`, the shared #2840 authority for "the
// substitution a base instantiation is reached under".
//
// ── WHAT IS STILL NOT FIXED, AND WHY THIS IS `narrow` ───────────────────────
//
// The §22.10 INHERITANCE-CLAUSE spelling — `class D(): B<()>(C.Nothing())` —
// still reports the #1386 ICE, and N7 PINS that it does. That site
// (`emitImplicitBase`'s `primaryBaseCtorArgs` loop) calls the base ctor through
// its ERASED template signature with no argument coercion at all, so it emits
// malformed IR for a nullable formal at a base with an explicit constructor
// whatever the type is — `class D(): B<int>(7)` over `constructor(u: T?)`, and
// even the wholly non-generic `class D(): B(7)` over `constructor(u: int?)`,
// both fail "Call parameter type does not match function signature" on this
// tree. Rescuing the unit argument there would turn a LOUD compile error into a
// malformed-IR failure or a silently wrong value, so the ICE is left standing
// and the underlying gap is filed as #3524. N7 exists so that a later fix has
// to come here and replace it with a value assertion, rather than the ICE
// quietly turning into a wrong answer.
//
// Two further shapes stay outside the rescue's reach, both measured and filed:
//
//   * A chain call written INSIDE a generic class's own constructor body —
//     `class B<T> … constructor() this(C.Nothing());` or `class D<U>: B<U> …
//     constructor() base(C.Nothing());`. There `T`/`U` is genuinely unbound
//     (this is the ERASED template body), so there is no correct value to
//     supply. It is a sema acceptance gap rather than a codegen one: the same
//     constructors accept `this("x")` at the same `T?` formal and fail as
//     malformed IR, and the unit spelling fails at `D<int>` exactly as it does
//     at `D<()>`. Filed as #3526, with both measurements.
//   * `new Inner<()>(x)` inside a generic scope that binds a type parameter of
//     the SAME NAME. `carryCalleeUnitSubst` deliberately refuses to overwrite
//     such a name — the argument expressions in that same loop are lowered
//     under the enclosing binding on purpose — so the loud #1386 stands there
//     too. Filed as #3525, with the plumbing fix it wants.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

/* §4.7's "methods declared without an explicit return type" — a producer of
   the unit value whose evaluation is observable. */
constexpr const char* kNothing =
    "  public static method Nothing() "
    "    Console.WriteLine(\"side\"); "
    "  end Nothing; ";

bool hasError(const bezel::test::JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── N1 — the issue's spelling A: `new B<()>(C.Nothing())` at a `u: T` formal.
// The argument's side effect must run, then the constructor body — exactly
// once each.
BEZEL_TEST(codegen_generic_unit_ctor_arg_3501,
           new_at_a_generic_unit_formal_spec_4_7_7_10) {
    auto r = runAot(
        "module M3501a; "
        "class B<T> where T: value "
        "  public constructor(u: T) Console.WriteLine(\"B-ctor\"); "
        "  end constructor; "
        "end B; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var b := new B<()>(C.Nothing()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nB-ctor\n"));
}

// ── N2 — the `()` LITERAL at the same formal. §4.7 gives the type "exactly one
// value, also written `()`"; the literal has always lowered to no
// `llvm::Value`, so this is a separate reach of the same rescue.
BEZEL_TEST(codegen_generic_unit_ctor_arg_3501,
           new_with_a_unit_literal_at_a_generic_formal_spec_4_7) {
    auto r = runAot(
        "module M3501b; "
        "class B<T> where T: value "
        "  public constructor(u: T) Console.WriteLine(\"B-ctor\"); "
        "  end constructor; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B<()>(()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("B-ctor\n"));
}

// ── N3 — the `T?` formal at `T = ()`, BOTH §4.7 values. This is the row a
// "fix" that stopped the ICE by handing over a zero cannot pass: the present
// value must read present and `null` must still read absent.
BEZEL_TEST(codegen_generic_unit_ctor_arg_3501,
           both_unit_optional_values_at_a_generic_formal_spec_4_7_7_10) {
    auto r = runAot(
        "module M3501c; "
        "class B<T> where T: value "
        "  public constructor(u: T?) "
        "    Console.WriteLine((u <> null).ToString()); "
        "  end constructor; "
        "end B; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var a := new B<()>(C.Nothing()); "
        "    using var b := new B<()>(null); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\nFalse\n"));
}

// ── N4 — the issue's spelling B: the `base(...)` chain into a concrete generic
// base. A DIFFERENT emitter path from N1's (`emitChainCall`, not
// `buildOrderedArgs`) with its own missing substitution, so its passing is not
// implied by N1's. §22.7: chaining "uses `this(...)` or `base(...)` in the
// chaining position before the body", which is the order pinned here.
BEZEL_TEST(codegen_generic_unit_ctor_arg_3501,
           base_chain_at_a_generic_unit_formal_spec_4_7_7_10_22_7) {
    auto r = runAot(
        "module M3501d; "
        "class B<T> where T: value "
        "  public constructor(u: T) Console.WriteLine(\"B-ctor\"); "
        "  end constructor; "
        "end B; "
        "class D: B<()> "
        "  public constructor() "
        "    base(C.Nothing()); "
        "    Console.WriteLine(\"D-ctor\"); "
        "  end constructor; "
        "end D; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nB-ctor\nD-ctor\n"));
}

// ── N5 — the same chain at a `T?` formal, both §4.7 values, through two
// derived classes so a single monomorphization of `B<()>` serves both.
BEZEL_TEST(codegen_generic_unit_ctor_arg_3501,
           base_chain_delivers_both_unit_optional_values_spec_4_7) {
    auto r = runAot(
        "module M3501e; "
        "class B<T> where T: value "
        "  public constructor(u: T?) "
        "    Console.WriteLine((u <> null).ToString()); "
        "  end constructor; "
        "end B; "
        "class D: B<()> "
        "  public constructor() base(C.Nothing()); end constructor; "
        "end D; "
        "class E: B<()> "
        "  public constructor() base(null); end constructor; "
        "end E; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    using var e := new E(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\nFalse\n"));
}

// ── N6 — the NON-UNIT control, and the reason the carry is gated to
// unit-shaped bindings. A `T?` formal at `T = int` must keep the call-site
// narrowing that `applyMethodGenericSubst` performs later — the #885/#886
// `GetVal<int>(null, 42)` path. Installing a non-unit binding early would strip
// the `?` and break exactly this row.
BEZEL_TEST(codegen_generic_unit_ctor_arg_3501,
           an_int_type_argument_at_the_same_formal_is_unchanged_control) {
    auto r = runAot(
        "module M3501f; "
        "class B<T> where T: value "
        "  public constructor(u: T?) "
        "    Console.WriteLine((u <> null).ToString()); "
        "  end constructor; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new B<int>(5); "
        "    using var b := new B<int>(null); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

// ── N7 — THE RESIDUAL, PINNED AS A LOUD FAILURE. `class D(): B<()>(...)` — the
// §22.10 inheritance-clause spelling — still reports the #1386 ICE. It is
// pinned by its exact diagnostic TEXT (not `hasErrors()`, which cannot tell
// this apart from any other error) so that the day it is fixed, this row has to
// be replaced with a VALUE assertion. The alternative — leaving it unpinned —
// is what would let a future change turn the loud error into a silent wrong
// value, which is the failure mode #3502 was.
//
// See the file header for why the rescue is deliberately NOT extended to that
// site: it calls the base constructor through the erased template signature
// with no argument coercion, and fails identically for `int?` and for a wholly
// non-generic base.
BEZEL_TEST(codegen_generic_unit_ctor_arg_3501,
           inheritance_clause_spelling_still_reports_the_loud_ice_residual) {
    auto r = runAot(
        "module M3501g; "
        "class B<T> where T: value "
        "  public constructor(u: T?) "
        "    Console.WriteLine((u <> null).ToString()); "
        "  end constructor; "
        "end B; "
        "class D(): B<()>(C.Nothing()) "
        "end D; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "lowerExpr returned null for positional base-constructor argument"));
    BEZEL_EXPECT_TRUE(hasError(r, "#1386"));
}
