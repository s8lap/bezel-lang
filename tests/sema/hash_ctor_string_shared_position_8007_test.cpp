// Spec-derived regression test for issue #8007 (spec §5.1, §17.7, §22.6;
// stdlib §4.8, §4.9, §4.10, v0.353).
//
// THE DEFECT. The hash collections' FIRST constructor position is SHARED:
// stdlib §4.10 declares, verbatim,
//
//     public constructor();
//     public constructor(capacity: int);
//     public constructor(comparer: IEqualityComparer<T>);
//     public constructor(capacity: int, comparer: IEqualityComparer<T>);
//
// on `Set<T>` (and, identically, on `OwningSet<T>`), and stdlib §4.8 / §4.9
// declare the same four on `Dictionary<K, V>` / `OwningDictionary<K, V>`. The
// one-argument position therefore carries EITHER the `capacity: int` OR the
// `comparer: IEqualityComparer<K>` -- the two declared types are told apart by
// the ARGUMENT's own type.
//
// A `string` at that position is NEITHER: it is not an `int` (§5.1's table has
// no `string` row, so no implicit conversion exists) and it is not an
// `IEqualityComparer<T>`. MEASURED on the base (r38-c2 at 95fe673f), the call
// drew the COMPARER DEFERRAL --
//
//     stdlib §4.10: the 'Set' IEqualityComparer constructor is not yet
//     implemented -- the comparer would be silently ignored ...
//
// -- which tells the reader that their `string` "is not a comparer", a
// statement about a type the argument never had. The right diagnostic is
// §5.1's: the `capacity` argument's type is not implicitly convertible to the
// target type.
//
// THE MECHANISM. `ExprTC::checkBuiltinCtorArgTypes`
// (src/sema/ExprTypecheck_Exprs.cpp) reads the `kinds` column of the ONE
// built-in parameter table (src/common/BuiltinRegistry.cpp) for the position.
// The hash collections' shared position carries `{kBPTInt, kBPTComparer}` --
// NO `kBPTString` -- while `StringBuilder`'s shared position carries
// `{kBPTInt, kBPTString}` (its `capacity: int` beside its `initial: string`),
// and a `string` IS legal there. The gate's "a `string` argument skips the
// whole position" shortcut (written for `StringBuilder` in fix #7820) was
// therefore applied to a position that admits no `string` at all, the §5.1
// gate never ran, and the call survived to CODEGEN, whose own comparer
// discriminator (`isCapacityPrimitive` in src/codegen/LlvmBackend_Exprs2.cpp,
// which does not include `String`) reported the deferral.
//
// THE FIX. Skip the position for a `string` argument only when the position's
// own kinds admit a `string`. On the hash collections the §5.1 gate then runs
// and the argument is refused as what it is.
//
// WHAT MUST NOT MOVE (fix #2722, the closed issue). A REAL
// `IEqualityComparer<T>` argument -- positional or NAMED -- must STILL be
// refused with the comparer deferral: codegen's `lowerNew` drops the comparer
// and builds the table with the default hash/equality trampolines, so that
// program must not silently run with the wrong equality. The control rows below
// pin both spellings.
//
// THE CONTROLS THAT MUST NOT MOVE. `new StringBuilder("x")` and
// `new StringBuilder(16)` stay accepted (stdlib §3.1's `StringBuilder`
// declares both the `initial: string` and the `capacity: int` forms), and
// `new Set<int>(16)` stays accepted (stdlib §4.10's `capacity: int`).
//
// THE ORACLE. `runAot` is the strict AOT oracle (fork-isolated
// `Driver::compileFile` + clang link + native exec). Every REFUSAL row asserts
// the POSITIVE diagnostic text at a REAL source location, and every row also
// asserts the ABSENCE of the wrong one -- on a program that does NOT compile,
// so `runAot` DOES fill `JitResult::diagnostics` (it fills them only on a
// reject, so an absence assertion on a COMPILED program would be vacuous).
//
// ASan is OFF in lanes by default (CLAUDE.md): this is an argument's type
// admission and a diagnostic, not allocation, layout, retain/release or
// disposal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A program over the hash collections (stdlib §4).
std::string setProg(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C "
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A program over the OWNING hash collections (stdlib §4.9 / §4.10). Their
// element type must be a class (`where T: class`), so a `Box` is declared.
std::string owningProg(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class Box public V: int; "
           "  public constructor(v: int) V := v; end constructor; "
           "  destructor end destructor; "   // §A.4: a destructor has no visibility
           "end Box; "
           "class C "
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A program over `StringBuilder` (stdlib §3.1).
std::string sbProg(const std::string& body) {
    return "module M; import System.Text; "
           "class C "
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A program carrying a REAL `IEqualityComparer<int>` (stdlib §4.8's
// `IEqualityComparer<T>`: `Equals(a: T, b: T): bool` and
// `GetHashCode(value: T): int`).
std::string cmpProg(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class ModCmp: IEqualityComparer<int> "
           "  public method Equals(a: int, b: int): bool => a mod 10 = b mod 10; "
           "  public method GetHashCode(value: int): int => value mod 10; "
           "end ModCmp; "
           "class C "
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §5.1 diagnostic for a `string` at a `capacity: int` position, named by
// the head it is about.
std::string capacityNotConvertible(const std::string& head) {
    return "the `capacity` argument of the `" + head +
           "` constructor has a type that is not implicitly convertible to the "
           "target type (\xC2\xA7""5.1)";
}

// The comparer deferral (fix #2722), named by the head it is about.
std::string comparerDeferral(const std::string& head) {
    return "the '" + head +
           "' IEqualityComparer constructor is not yet implemented";
}

// A refusal row: the program must NOT run, the diagnostic must carry `needle`
// at a REAL source location, and the WRONG diagnostic must be absent.
void expectRefusedWith(const std::string& src, const std::string& needle,
                        const std::string& absent = std::string()) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
    if (!absent.empty())
        BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, absent.c_str()));
}

// An accept row: the program must compile CLEAN, run, and exit 0.
void expectAccepted(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace

// ===========================================================================
// #8007 -- the four spellings. A `string` at the SHARED capacity-or-comparer
// position is refused with §5.1's type diagnostic, not with the comparer
// deferral.
// ===========================================================================

// per spec §5.1 (no implicit conversion from `string` to `int`) + stdlib §4.10
// (`Set<T>(capacity: int)` beside `Set<T>(comparer: IEqualityComparer<T>)`):
// the `Set` spelling.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           set_string_at_shared_position_refused_with_5_1) {
    expectRefusedWith(
        setProg("  using var s: Set<int> := new Set<int>(\"x\"); "
                "  return s.Count; "),
        capacityNotConvertible("Set"),
        // The MIS-DIAGNOSIS the base drew: a `string` is not a comparer.
        comparerDeferral("Set"));
}

// per spec §5.1 + stdlib §4.8 (`Dictionary<K, V>`): the `Dictionary` spelling.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           dictionary_string_at_shared_position_refused_with_5_1) {
    expectRefusedWith(
        setProg("  using var d: Dictionary<string, int> := "
                "    new Dictionary<string, int>(\"x\"); "
                "  return d.Count; "),
        capacityNotConvertible("Dictionary"),
        comparerDeferral("Dictionary"));
}

// per spec §5.1 + stdlib §4.10 (`OwningSet<T>`): the owning-set spelling.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           owning_set_string_at_shared_position_refused_with_5_1) {
    expectRefusedWith(
        owningProg("  using var s: OwningSet<Box> := "
                   "    new OwningSet<Box>(\"x\"); "
                   "  return s.Count; "),
        capacityNotConvertible("OwningSet"),
        comparerDeferral("OwningSet"));
}

// per spec §5.1 + stdlib §4.9 (`OwningDictionary<K, V>`): the owning-dictionary
// spelling.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           owning_dictionary_string_at_shared_position_refused_with_5_1) {
    expectRefusedWith(
        owningProg("  using var d: OwningDictionary<int, Box> := "
                   "    new OwningDictionary<int, Box>(\"x\"); "
                   "  return d.Count; "),
        capacityNotConvertible("OwningDictionary"),
        comparerDeferral("OwningDictionary"));
}

// ===========================================================================
// THE CONTROLS THAT MUST NOT MOVE (fix #2722, closed).
// ===========================================================================

// per stdlib §4.10 + fix #2722: a REAL `IEqualityComparer<T>` argument is
// STILL refused with the comparer deferral -- codegen's `lowerNew` drops the
// comparer and builds the table with the default hash/equality trampolines, so
// the program must not run with the wrong equality. The §5.1 capacity
// diagnostic must NOT be drawn instead.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           real_comparer_still_refused_with_the_comparer_deferral) {
    expectRefusedWith(
        cmpProg("  using var cmp: ModCmp := new ModCmp(); "
                "  using var s: Set<int> := new Set<int>(cmp); "
                "  return s.Count; "),
        comparerDeferral("Set"),
        capacityNotConvertible("Set"));
}

// per stdlib §4.10 + spec §22.5 (a named argument binds the parameter it
// names) + fix #2722: the NAMED comparer spelling is the same refusal.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           named_comparer_still_refused_with_the_comparer_deferral) {
    expectRefusedWith(
        cmpProg("  using var cmp: ModCmp := new ModCmp(); "
                "  using var s: Set<int> := new Set<int>(comparer := cmp); "
                "  return s.Count; "),
        comparerDeferral("Set"),
        capacityNotConvertible("Set"));
}

// per stdlib §3.1 (`StringBuilder`): the `initial: string` form is LEGAL —
// `StringBuilder`'s shared position admits a `string`, so the §5.1 gate must
// not refuse it. This is the control for the skip the fix narrows.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           string_builder_string_argument_still_accepted) {
    expectAccepted(sbProg(
        "  using var sb: StringBuilder := new StringBuilder(\"x\"); "
        "  return 0; "));
}

// per stdlib §3.1 (`StringBuilder(capacity: int)`): the capacity form is legal
// too.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           string_builder_capacity_argument_still_accepted) {
    expectAccepted(sbProg(
        "  using var sb: StringBuilder := new StringBuilder(16); "
        "  return 0; "));
}

// per stdlib §4.10 (`Set<T>(capacity: int)`): the capacity form is legal, and
// the fix must not refuse it.
BEZEL_TEST(hash_ctor_string_shared_position_8007,
           set_int_capacity_still_accepted) {
    expectAccepted(setProg(
        "  using var s: Set<int> := new Set<int>(16); "
        "  return 0; "));
}
