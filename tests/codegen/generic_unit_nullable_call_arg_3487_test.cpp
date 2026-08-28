// §4.7 "The unit type" / §14.2 "Constraints" (v0.283): a generic method whose
// formal is `T?` under `where T: value`, monomorphized at `T = ()`.
//
// This is the exact program #3487 was measured on. It is BEHAVIOURAL COVERAGE
// of the `nullableTypeParamConcrete` arm of `buildOrderedArgs`'s unit rescue
// (src/codegen/LlvmBackend_Assign.cpp) — the arm that resolves a bare `T?`
// formal to its concrete `()?` before asking `unitValueForTarget` for an
// operand. The existing #3095/#3137 rows in
// tests/codegen/unit_optional_signature_3095_test.cpp cover the FUNCTION-VALUE
// spellings (`(()?) -> int`) and `Id<()>(())`; none of them reaches this arm,
// because none has a bare type-parameter formal that needs resolving.
//
// ⚠ HONEST SCOPE. This test does NOT pin #3487's actual defect. That defect
// was a `stack-use-after-scope` INSIDE `bezelc` — the concrete-type holder was
// scoped to an inner `if` while the pointer that pointed at it was not — and
// this program printed the CORRECT answers both before and after the fix,
// because the dead stack slot still held the right bytes. No behavioural
// oracle in this repo can fail on compiler-internal UB: the suite's
// generated-code AddressSanitizer instruments the emitted program, not the
// compiler. The mechanical pin for #3487 is the source-shape ratchet in
// tests/support/typeref_holder_scope_escape_3487_test.cpp, which IS measured
// failing on the pre-fix source. This row exists so the branch has a
// behavioural anchor at all: if a later refactor of that rescue drops the
// resolution, these values change and this fails.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"

#include "test_harness.hpp"

using bezel::test::runAot;

BEZEL_TEST(generic_unit_nullable_call_arg_3487,
           a_unit_valued_call_at_a_generic_nullable_formal_is_present_spec_4_7) {
    // `C.Take<()>(C.Nothing())`: `Nothing()` is declared with no return type,
    // so per §4.7 it produces the unit value. At the `T?` formal with
    // `T = ()` that is the PRESENT `()?`, so `x = null` is false and Take
    // returns 1. The `side` line proves the argument's side effect still runs
    // — the rescue substitutes an operand, it does not re-emit or drop the
    // call that produced it.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Take<T>(x: T?): int where T: value "
        "    return (if x = null then 0 else 1); "
        "  end Take; "
        "  public static method Nothing() Console.WriteLine(\"side\"); "
        "end Nothing; "
        "  public static method Main(): int "
        "    Console.WriteLine(C.Take<()>(C.Nothing()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("side") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1") != std::string::npos);
    // The distinction must survive: a present `()?` is NOT the absent one.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("0") == std::string::npos);
}

BEZEL_TEST(generic_unit_nullable_call_arg_3487,
           a_null_literal_at_the_same_generic_nullable_formal_is_absent_spec_4_7) {
    // The other §4.7 value at the same formal. Pinned beside the present case
    // so a rescue that always supplies the present wrap cannot pass: this one
    // must print 0.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Take<T>(x: T?): int where T: value "
        "    return (if x = null then 0 else 1); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(C.Take<()>(null).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("0") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1") == std::string::npos);
}

BEZEL_TEST(generic_unit_nullable_call_arg_3487,
           both_unit_optional_values_reach_the_same_generic_formal_spec_4_7) {
    // Both in one program, in the issue's own order, so the two lowerings are
    // exercised in a single monomorphization of `Take<()>`.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Take<T>(x: T?): int where T: value "
        "    return (if x = null then 0 else 1); "
        "  end Take; "
        "  public static method Nothing() Console.WriteLine(\"side\"); "
        "end Nothing; "
        "  public static method Main(): int "
        "    Console.WriteLine(C.Take<()>(C.Nothing()).ToString()); "
        "    Console.WriteLine(C.Take<()>(null).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    const std::size_t side = r.stdoutText.find("side");
    const std::size_t one = r.stdoutText.find("1", side == std::string::npos
                                                       ? 0
                                                       : side);
    const std::size_t zero = r.stdoutText.find("0", one == std::string::npos
                                                        ? 0
                                                        : one);
    BEZEL_EXPECT_TRUE(side != std::string::npos);
    BEZEL_EXPECT_TRUE(one != std::string::npos);
    BEZEL_EXPECT_TRUE(zero != std::string::npos);
    BEZEL_EXPECT_TRUE(side < one);
    BEZEL_EXPECT_TRUE(one < zero);
}
