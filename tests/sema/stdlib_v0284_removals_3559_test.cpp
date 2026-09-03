// fix #3559 — the compiler catch-up to the v0.284 subtraction batch
// (stdlib §2.2 `Math`, stdlib §5 `System.Collections.Linq`; documents at
// v0.285).
//
// WHAT v0.284 REMOVED, and what this file pins.
//
// Proposal 0121 R3 withdrew the `Math` TRANSCENDENTAL BLOCK from stdlib §2.2 —
// `Exp`, `Log` (both overloads), `Log10`, `Log2`, `Sin`, `Cos`, `Tan`, `Asin`,
// `Acos`, `Atan`, `Atan2`, `Sinh`, `Cosh`, `Tanh`, and the `E` constant. The
// coupled G1 ruling (issue 3411) withdrew `Hypot`, `Cbrt` and `PI` with it:
// those three were implemented but NEVER documented, `PI` being a duplicate
// spelling of the documented `Pi`. R2 withdrew `OfType<U>()` from stdlib §5.
//
// Until this fix the compiler still accepted all of them, so they were
// impl-ahead-of-spec deltas. Each row below is a REJECTION regression test.
//
// WHY THE DIAGNOSTIC TEXT MATTERS AS MUCH AS THE REJECTION. A member that was
// REMOVED and a member that never existed are different facts, and only one of
// them is worth re-reading your source for a typo over. Every reject row here
// therefore asserts the removal wording and the revision — not `hasErrors()`,
// which cannot tell this rule from an unrelated unknown-member error, and which
// a pre-existing "'Math' has no static method or property 'Cbrt'" would already
// have satisfied for `Cbrt`.
//
// THE ACCEPT-CONTROLS ARE THE POINT OF THE FILE. This is a SUBTRACTION, so the
// live risk is over-rejection of the members that STAYED (stdlib §2.2 keeps
// `Sqrt`, `Pow`, `DivRem`, `Fmod`, `IEEERemainder`, the rounding family, `Abs`,
// `Sign`, `Min`/`Max`/`Clamp`, and the constants `Pi` and `Tau`). Every control
// asserts a VALUE, never "it compiled": `Fmod` and `IEEERemainder` in
// particular share the one 2-arg lowering that `Atan2` and `Hypot` were deleted
// out of, and a control that only proved the compiler did not error could not
// tell a preserved lowering from one that now calls the wrong libc function.
//
// issue 3440's interim `OfType` rejection ("not yet implemented", raised only
// for the pairs whose identity relabelling could not be proven sound) is
// SUPERSEDED here: the interim wording is gone and the removal wording is the
// one spelling both sema arms raise. The rows named `..._was_accepted_at_v0283`
// are exactly the cells that interim let through, which makes them the rows
// that can only pass on a compiler carrying this fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The hierarchy the `OfType` rows share. `Dog` derives from `Animal` and
// implements `IBarker`; `Cat` derives from `Animal` and does not.
const char* kTypes = R"BZL(
module M;
import System;
public interface IBarker
  method Bark(): int;
end IBarker;
public class Animal
  public Tag: int := 1;
  public virtual method Who(): int return 0; end Who;
end Animal;
public class Dog: Animal, IBarker
  public DogOnly: int := 77;
  public override method Who(): int return 1; end Who;
  public method Bark(): int return 7; end Bark;
end Dog;
public class Cat: Animal
  public override method Who(): int return 2; end Who;
end Cat;
public class Box<T>
  public Tag: int := 3;
end Box;
)BZL";

std::string prog(const std::string& mainBody) {
    return std::string(kTypes) +
           "class C\n"
           "  public static method Main(): int\n" +
           mainBody +
           "\n  end Main;\nend C;\n";
}

// A bare `module M; class C ... end C;` shell for the `Math` rows, which need
// no hierarchy.
std::string mathProg(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

JitResult reject(const std::string& src) { return runAot(src); }

// True iff SOME error diagnostic is the v0.284 REMOVAL diagnostic for
// `Math.<member>` — carrying the qualified name, the removal framing, the
// revision it happened at, the stdlib section, and the issue pointer.
bool removedMathMember(const JitResult& r, const std::string& qualified) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const std::string& m = d.message;
        if (m.find("'" + qualified + "' was REMOVED from the standard library "
                   "at v0.284 (stdlib §2.2)") == std::string::npos)
            continue;
        if (m.find("it is gone, not misspelled (issue 3559)") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

// The same, for the stdlib §5 `OfType<U>()` removal, naming BOTH types so the
// reader can see which call was refused.
bool removedOfType(const JitResult& r, const std::string& u,
                   const std::string& t) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const std::string& m = d.message;
        if (m.find("`OfType<" + u + ">()` on a sequence of `" + t + "`") ==
            std::string::npos)
            continue;
        if (m.find("was REMOVED from the standard library at v0.284 "
                   "(stdlib §5)") == std::string::npos)
            continue;
        if (m.find("the operator is gone, not misspelled (issue 3559)") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

// A weaker sibling for the one row whose SOURCE element type is a generic
// (`Box<int>`), whose rendering in the diagnostic is `typeNameForDiag`'s
// business and not this test's contract. Still text-based, still the removal
// wording, still the operator and its `U` — only the source spelling is left
// unpinned.
bool removedOfTypeUnnamedPair(const JitResult& r, const std::string& u) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const std::string& m = d.message;
        if (m.find("`OfType<" + u + ">()` on a sequence of `") ==
            std::string::npos)
            continue;
        if (m.find("was REMOVED from the standard library at v0.284 "
                   "(stdlib §5)") == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// The interim issue-3440 wording must be GONE — one spelling, not two. A
// compiler that still raised "is not yet implemented" for some pair would leave
// two diagnostics claiming to decide the same thing.
bool carriesInterim3440Wording(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is not yet implemented (stdlib §5 / issue 3440)") !=
            std::string::npos)
            return true;
    return false;
}

// The diagnostic must point at the call, never at 0:0.
//
// It cannot be checked through `d.location`: on a compile REJECT the AOT
// harness synthesizes exactly ONE diagnostic whose `location` is a
// default-constructed `SourceLocation` (0:0) and whose `message` is the whole
// captured stderr. The rendered `<file>:<line>:<col>:` prefix inside the
// message is the real location and the one the user sees, so that is what this
// reads: both numbers present and non-zero immediately before `needle`.
bool locatedAt(const JitResult& r, const std::string& needle) {
    const std::string tail = ": " + needle;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        for (std::size_t at = m.find(tail); at != std::string::npos;
             at = m.find(tail, at + 1)) {
            std::size_t p = at;
            auto digitsBack = [&m](std::size_t& q) -> long {
                const std::size_t end = q;
                while (q > 0 && m[q - 1] >= '0' && m[q - 1] <= '9') --q;
                if (q == end) return -1;
                return std::stol(m.substr(q, end - q));
            };
            const long col = digitsBack(p);
            if (col <= 0 || p == 0 || m[p - 1] != ':') continue;
            --p;
            if (digitsBack(p) > 0) return true;
        }
    }
    return false;
}

}  // namespace

// ===========================================================================
// REJECT — stdlib §2.2 `Math`, the v0.284 transcendental block (v0.285)
// ===========================================================================

#define BEZEL_MATH_REMOVED_1ARG(rowname, member)                              \
    BEZEL_TEST(sema_v0284_removals_3559, rowname) {                           \
        auto r = reject(mathProg("    var x := Math." #member "(1.0);\n"      \
                                 "    return 0;"));                           \
        BEZEL_EXPECT_FALSE(r.ran);                                            \
        BEZEL_EXPECT_TRUE(removedMathMember(r, "Math." #member));             \
    }

BEZEL_MATH_REMOVED_1ARG(reject_math_exp_v0284, Exp)
BEZEL_MATH_REMOVED_1ARG(reject_math_log_v0284, Log)
BEZEL_MATH_REMOVED_1ARG(reject_math_log2_v0284, Log2)
BEZEL_MATH_REMOVED_1ARG(reject_math_log10_v0284, Log10)
BEZEL_MATH_REMOVED_1ARG(reject_math_sin_v0284, Sin)
BEZEL_MATH_REMOVED_1ARG(reject_math_cos_v0284, Cos)
BEZEL_MATH_REMOVED_1ARG(reject_math_tan_v0284, Tan)
BEZEL_MATH_REMOVED_1ARG(reject_math_asin_v0284, Asin)
BEZEL_MATH_REMOVED_1ARG(reject_math_acos_v0284, Acos)
BEZEL_MATH_REMOVED_1ARG(reject_math_atan_v0284, Atan)
BEZEL_MATH_REMOVED_1ARG(reject_math_sinh_v0284, Sinh)
BEZEL_MATH_REMOVED_1ARG(reject_math_cosh_v0284, Cosh)
BEZEL_MATH_REMOVED_1ARG(reject_math_tanh_v0284, Tanh)
// G1 (issue 3411): never documented, removed with the block.
BEZEL_MATH_REMOVED_1ARG(reject_math_cbrt_g1_3411, Cbrt)

#undef BEZEL_MATH_REMOVED_1ARG

// The two-argument members of the block. `Atan2` is a documented removal;
// `Hypot` is a G1 ghost. Both were lowered through the SAME 2-arg libc arm that
// `Fmod` and `IEEERemainder` still use, so these two rows and the `Fmod` /
// `IEEERemainder` controls below are the pair that pins the arm was narrowed
// rather than deleted.
BEZEL_TEST(sema_v0284_removals_3559, reject_math_atan2_v0284) {
    auto r = reject(mathProg("    var x := Math.Atan2(1.0, 2.0);\n"
                             "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedMathMember(r, "Math.Atan2"));
}

BEZEL_TEST(sema_v0284_removals_3559, reject_math_hypot_g1_3411) {
    auto r = reject(mathProg("    var x := Math.Hypot(3.0, 4.0);\n"
                             "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedMathMember(r, "Math.Hypot"));
}

// The `E` constant. Two spellings reach it — the CALL-shaped constant path in
// `LlvmBackend_Calls.cpp` and the member-access path in `LlvmBackend_Exprs.cpp`
// — and sema now rejects before either runs.
BEZEL_TEST(sema_v0284_removals_3559, reject_math_e_constant_v0284) {
    auto r = reject(mathProg("    var x := Math.E;\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedMathMember(r, "Math.E"));
    BEZEL_EXPECT_TRUE(locatedAt(r, "'Math.E' was REMOVED"));
}

// The `PI` spelling. G1: never documented — `Pi` is the documented constant and
// stays, so this row and `accept_math_pi_constant_stays` are a matched pair.
BEZEL_TEST(sema_v0284_removals_3559, reject_math_uppercase_pi_g1_3411) {
    auto r = reject(mathProg("    var x := Math.PI;\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedMathMember(r, "Math.PI"));
}

// The diagnostic tells the reader what SURVIVED, which is the difference
// between "this member is gone" and "arithmetic is gone".
BEZEL_TEST(sema_v0284_removals_3559, removal_diagnostic_names_the_survivors) {
    auto r = reject(mathProg("    var x := Math.Sin(1.0);\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    bool namesSurvivors = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`Sqrt`, `Pow`, `DivRem`, `Fmod`, `IEEERemainder`, "
                           "the rounding family and the constants `Pi` and "
                           "`Tau` remain") != std::string::npos)
            namesSurvivors = true;
    BEZEL_EXPECT_TRUE(namesSurvivors);
}

// ===========================================================================
// ACCEPT-CONTROLS — stdlib §2.2's survivors keep compiling AND keep their
// values. Over-rejection is the live risk of a subtraction.
//
// The oracle is STDOUT, not the exit status: `runAot(...).exitCode` is the
// process exit status and truncates to 8 bits (`aot_support.hpp`), so a packed
// value like `int(Sqrt(16))*10000 + int(Pow(2,10))` would compare against a
// number the harness can never report. `runAotOut` strips ONE trailing
// newline, which is why each expectation below ends without one.
// ===========================================================================

BEZEL_TEST(sema_v0284_removals_3559, accept_math_sqrt_pow_stay) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
                        "    Console.WriteLine(int(Math.Sqrt(16.0)));\n"
                        "    Console.WriteLine(int(Math.Pow(2.0, 10.0)));\n"
                        "    return 0;")),
                    "4\n1024");
}

// The 2-arg libc arm, narrowed from four members to two. `Fmod(7,3) = 1` and
// `IEEERemainder(7,3) = 1`; `Fmod(-7,3) = -1` while `IEEERemainder(-7,3) = -1`
// too, so the discriminating pair is `Fmod(5,3) = 2` vs
// `IEEERemainder(5,3) = -1` — a row that would still pass if the arm had been
// left calling `remainder` for both would be no control at all.
BEZEL_TEST(sema_v0284_removals_3559, accept_math_fmod_and_ieeeremainder_stay) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
                        "    Console.WriteLine(int(Math.Fmod(5.0, 3.0)));\n"
                        "    Console.WriteLine(int(Math.IEEERemainder(5.0, "
                        "3.0)));\n"
                        "    return 0;")),
                    "2\n-1");
}

BEZEL_TEST(sema_v0284_removals_3559, accept_math_pi_constant_stays) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
                        "    Console.WriteLine(int(Math.Pi * 100.0));\n"
                        "    return 0;")),
                    "314");
}

BEZEL_TEST(sema_v0284_removals_3559, accept_math_tau_constant_stays) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
                        "    Console.WriteLine(int(Math.Tau * 100.0));\n"
                        "    return 0;")),
                    "628");
}

BEZEL_TEST(sema_v0284_removals_3559, accept_math_rounding_family_stays) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
                        "    Console.WriteLine(int(Math.Floor(2.7)));\n"
                        "    Console.WriteLine(int(Math.Ceiling(2.1)));\n"
                        "    Console.WriteLine(int(Math.Round(2.5)));\n"
                        "    Console.WriteLine(int(Math.Truncate(2.9)));\n"
                        "    return 0;")),
                    "2\n3\n2\n2");
}

BEZEL_TEST(sema_v0284_removals_3559, accept_math_abs_sign_minmax_clamp_stay) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
                        "    Console.WriteLine(Math.Abs(0 - 3));\n"
                        "    Console.WriteLine(Math.Sign(0 - 9));\n"
                        "    Console.WriteLine(Math.Min(4, 7));\n"
                        "    Console.WriteLine(Math.Clamp(9, 1, 5));\n"
                        "    return 0;")),
                    "3\n-1\n4\n5");
}

// ===========================================================================
// REJECT — stdlib §5 `OfType<U>()` (v0.284 R2)
//
// Every row named `..._was_accepted_at_v0283` is a cell issue 3440's interim
// predicate ADMITTED. Those are the rows that can only pass on a compiler
// carrying this fix; the two `..._was_rejected_at_v0283` rows pin that the
// interim wording was replaced rather than joined.
// ===========================================================================

BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_same_element_type_was_accepted_at_v0283) {
    auto r = reject(prog("    using var a := new Array<int>();\n"
                         "    a.Add(1); a.Add(2); a.Add(3);\n"
                         "    using var b := a.OfType<int>().ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
    BEZEL_EXPECT_TRUE(locatedAt(r, "`OfType<int>()`"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_base_class_upcast_was_accepted_at_v0283) {
    auto r = reject(prog("    using var xs := new OwningArray<Dog>();\n"
                         "    xs.Add(new Dog());\n"
                         "    var acc := 0;\n"
                         "    for var a in xs.OfType<Animal>() do\n"
                         "      acc := acc + a.Tag;\n"
                         "    end;\n"
                         "    return acc;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "Animal", "Dog"));
}

BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_interface_upcast_was_accepted_at_v0283) {
    auto r = reject(prog("    using var xs := new OwningArray<Dog>();\n"
                         "    xs.Add(new Dog());\n"
                         "    var acc := 0;\n"
                         "    for var b in xs.OfType<IBarker>() do\n"
                         "      acc := acc + b.Bark();\n"
                         "    end;\n"
                         "    return acc;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "IBarker", "Dog"));
}

BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_class_to_object_was_accepted_at_v0283) {
    auto r = reject(prog("    using var xs := new OwningArray<Dog>();\n"
                         "    xs.Add(new Dog());\n"
                         "    var n := 0;\n"
                         "    for var o in xs.OfType<Object>() do\n"
                         "      n := n + 1;\n"
                         "    end;\n"
                         "    return n;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "Object", "Dog"));
}

BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_user_generic_to_object_was_accepted_at_v0283) {
    auto r = reject(prog("    using var xs := new OwningArray<Box<int>>();\n"
                         "    xs.Add(new Box<int>());\n"
                         "    var n := 0;\n"
                         "    for var o in xs.OfType<Object>() do\n"
                         "      n := n + 1;\n"
                         "    end;\n"
                         "    return n;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfTypeUnnamedPair(r, "Object"));
}

// The LAZY-receiver arm — the second of the two sema arms that type `OfType`.
// A removal wired into only one arm would read as complete while half the
// operator's surface still lowered.
BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_lazy_arm_same_element_type_was_accepted_at_v0283) {
    auto r = reject(prog("    using var a := new Array<int>();\n"
                         "    a.Add(1); a.Add(2);\n"
                         "    using var b := a.Where(lambda(x: int) => x > 0)"
                         ".OfType<int>().ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_lazy_arm_upcast_was_accepted_at_v0283) {
    auto r = reject(prog("    using var xs := new OwningArray<Dog>();\n"
                         "    xs.Add(new Dog());\n"
                         "    var acc := 0;\n"
                         "    for var a in xs.Where(lambda(x: Dog) => true)"
                         ".OfType<Animal>() do\n"
                         "      acc := acc + a.Tag;\n"
                         "    end;\n"
                         "    return acc;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "Animal", "Dog"));
}

// A `Range<T>` receiver at what used to be its sound cell (§16.3, the same
// lazy arm).
BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_range_receiver_was_accepted_at_v0283) {
    auto r = reject(prog("    var n := 0;\n"
                         "    for var x in (1..4).OfType<int>() do "
                         "n := n + x; end;\n"
                         "    return n;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

// The two cells issue 3440's interim ALREADY rejected. They must still be
// rejected, and with the REMOVAL wording — not the interim one. That is the
// "one spelling, not two" half of this fix.
BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_downcast_was_rejected_at_v0283) {
    auto r = reject(prog("    using var xs := new OwningArray<Animal>();\n"
                         "    xs.Add(new Cat());\n"
                         "    var n := 0;\n"
                         "    for var d in xs.OfType<Dog>() do "
                         "n := n + d.DogOnly; end;\n"
                         "    return n;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "Dog", "Animal"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

BEZEL_TEST(sema_v0284_removals_3559,
           reject_oftype_int_to_string_was_rejected_at_v0283) {
    auto r = reject(prog("    using var a := new Array<int>();\n"
                         "    a.Add(1);\n"
                         "    using var b := a.OfType<string>().ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "string", "int"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

// ===========================================================================
// REJECT — issue 3710: `OfType<U>(x)` at ANY arity (fix #3710, stdlib §5,
// v0.286).
//
// Both removal arms above gated on `ce.args.empty()`, so every row in this
// section (arity != 0, across every receiver shape the two arms cover) used
// to be WAVED PAST the removal diagnostic — the §5 name exemption in
// `kBuiltinCollectionLinqOperatorNames` (BuiltinRegistry.cpp) that lets a bare
// `OfType` reference reach this arm at all is name-only, not arity-aware, so
// nothing else typed the call and codegen's #2434/#2491 zero-init
// degradation notes let it compile rc=0, link, and run silently. fix #3710
// drops the `ce.args.empty()` guard from both arms (ExprTypecheck_Calls.cpp
// ~:4733 lazy-receiver arm, ~:6265 collection-receiver arm) — a withdrawn
// member is a removal diagnostic at ANY arity, matching the zero-arg rows
// above.
//
// §6b exclusion diff: `ce.args.empty()` previously ADMITTED every non-empty
// arg list (1 arg, 2 args, with or without an explicit `<U>`) on EVERY
// receiver shape both arms cover — concrete collection (Array, Set),
// lazy-chain, Range, and an `IEnumerable<T>`-typed parameter. Every row below
// is one of those previously-admitted cells; each is now a reject, matching
// the zero-arg row for the same receiver shape above. No accept row in this
// file flips: the user-declared `Bag.OfType(n)` control
// (`oftype_removal_completeness_3440_test.cpp`'s
// `accept_user_declared_oftype_method_still_runs`) is untouched by this
// change because it never reaches either removal arm (its receiver is `Bag`,
// not an `IEnumerable<T>` builtin).
// ===========================================================================

BEZEL_TEST(sema_v0284_removals_3559,
           reject_3710_oftype_one_argument_collection_arm) {
    auto r = reject(prog("    using var a := new Array<int>();\n"
                         "    a.Add(1); a.Add(2); a.Add(3);\n"
                         "    using var b := a.OfType<int>(0).ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
    BEZEL_EXPECT_TRUE(locatedAt(r, "`OfType<int>()`"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

BEZEL_TEST(sema_v0284_removals_3559, reject_3710_oftype_two_arguments) {
    auto r = reject(prog("    using var a := new Array<int>();\n"
                         "    a.Add(1);\n"
                         "    using var b := a.OfType<int>(1, 2).ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

// No explicit `<U>` type argument AND a value argument — the source element
// type stands in for U (same fallback the zero-arg, no-typearg cell would
// use), so `removedOfType` is asserted with `int`/`int`.
BEZEL_TEST(sema_v0284_removals_3559, reject_3710_oftype_no_typearg_with_arg) {
    auto r = reject(prog("    using var a := new Array<int>();\n"
                         "    a.Add(1);\n"
                         "    using var b := a.OfType(1).ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

// The upcast shape (base-class `<U>`) WITH an argument — combines 3710's
// arity gap with 3440's original identity-relabelling grid, on the receiver
// family the issue's own repro used (`OwningArray<Animal>`).
BEZEL_TEST(sema_v0284_removals_3559,
           reject_3710_oftype_upcast_with_argument) {
    auto r = reject(prog("    using var xs := new OwningArray<Dog>();\n"
                         "    xs.Add(new Dog());\n"
                         "    var acc := 0;\n"
                         "    for var a in xs.OfType<Animal>(0) do\n"
                         "      acc := acc + a.Tag;\n"
                         "    end;\n"
                         "    return acc;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "Animal", "Dog"));
}

// The LAZY-receiver arm (the sibling `ce.args.empty()` guard this fix also
// drops) WITH an argument.
BEZEL_TEST(sema_v0284_removals_3559, reject_3710_oftype_lazy_arm_with_argument) {
    auto r = reject(prog("    using var a := new Array<int>();\n"
                         "    a.Add(1); a.Add(2);\n"
                         "    using var b := a.Where(lambda(x: int) => x > 0)"
                         ".OfType<int>(0).ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
    BEZEL_EXPECT_FALSE(carriesInterim3440Wording(r));
}

// A `Set<T>` receiver — the collection-receiver arm's OTHER concrete builtin
// (not just `Array`/`OwningArray`) — WITH an argument.
BEZEL_TEST(sema_v0284_removals_3559, reject_3710_oftype_set_receiver_with_argument) {
    auto r = reject(prog("    using var s := new Set<int>();\n"
                         "    s.Add(1);\n"
                         "    using var b := s.OfType<int>(0).ToArray();\n"
                         "    return b.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

// An `IEnumerable<T>`-typed PARAMETER receiver WITH an argument — the shape
// that is neither a concrete collection nor a lazy chain.
BEZEL_TEST(sema_v0284_removals_3559,
           reject_3710_oftype_ienumerable_param_receiver_with_argument) {
    auto r = reject(std::string(kTypes) +
                    "class C\n"
                    "  public static method F(xs: IEnumerable<int>): int\n"
                    "    using var b := xs.OfType<int>(0).ToArray();\n"
                    "    return b.Count;\n"
                    "  end F;\n"
                    "  public static method Main(): int\n"
                    "    using var a := new Array<int>();\n"
                    "    a.Add(1);\n"
                    "    return F(a);\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

// A `Range<T>` receiver (the lazy arm's third route) WITH an argument.
BEZEL_TEST(sema_v0284_removals_3559,
           reject_3710_oftype_range_receiver_with_argument) {
    auto r = reject(prog("    var n := 0;\n"
                         "    for var x in (1..4).OfType<int>(0) do "
                         "n := n + x; end;\n"
                         "    return n;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedOfType(r, "int", "int"));
}

// ===========================================================================
// ACCEPT-CONTROLS — stdlib §5's other lazy operators are untouched. `Where`
// shares the operator table `OfType` was removed from, and `Reverse` sits
// immediately beside it in the same `isOp` disjunction that lost the `OfType`
// term, so both are direct over-rejection controls for that edit.
// ===========================================================================

BEZEL_TEST(sema_v0284_removals_3559, accept_where_and_reverse_still_lower) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    using var a := new Array<int>();\n"
                        "    a.Add(1); a.Add(2); a.Add(3); a.Add(4);\n"
                        "    using var b := a.Where(lambda(x: int) => x > 1)"
                        ".Reverse().ToArray();\n"
                        "    Console.WriteLine(b.Count);\n"
                        "    Console.WriteLine(b[0]);\n"
                        "    return 0;")),
                    "3\n4");
}

BEZEL_TEST(sema_v0284_removals_3559, accept_select_still_lowers) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    using var a := new Array<int>();\n"
                        "    a.Add(1); a.Add(2); a.Add(3);\n"
                        "    using var b := a.Select(lambda(x: int) => x * 2)"
                        ".ToArray();\n"
                        "    Console.WriteLine(b.Count);\n"
                        "    Console.WriteLine(b[2]);\n"
                        "    return 0;")),
                    "3\n6");
}
