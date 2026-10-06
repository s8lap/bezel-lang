// fix #3462 (v0.315) — spec §8.6, §9.8, §22.3, §22.4.
//
// THE DEFECT. `isNonNullableReferenceType` decided "is this a non-nullable
// reference type" for a Named type by testing `kind == Class ||
// kind == Interface`. But `SemaContext::builtinClassSym` mints value types
// (Range, Ptr) with `kind = Class` unconditionally, so these value types
// were misclassified as reference types.
//
// THE FIX. Builtin value types have `moduleName.empty()` (no real module),
// so check that before the kind test. This fixes the misclassification at
// its single source, fixing all four consumers:
//   - codegen/LlvmBackend_Calls2.cpp: call-site non-nullable-ref treatment
//   - codegen/LlvmBackend_Calls2.cpp: out-param recipient path (§22.3)
//   - sema/ExprTypecheck_Exprs.cpp: definite-assignment for method-call out var
//   - sema/ExprTypecheck_Exprs.cpp: definite-assignment for ctor-call out var
//
// THE TEST. Range<T> is a value type with a default (zero).
// An `out var r` at a Range parameter should be treated as a VALUE type,
// meaning a read after a short-circuit path (where the call was skipped)
// is ACCEPTED — not a definite-assignment error.

#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckDA;

namespace {

// Test that Range<int> out var is treated as a VALUE type (default-init,
// no definite-assignment error when read after a short-circuit path).
BEZEL_TEST(sema_da,
           inline_out_var_range_value_type_read_after_short_circuit_accepted_spec_22_4_3462) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  static method TryGetRange(out r: Range<int>): bool "
        "    r := 1 .. 5; "
        "    return true; "
        "  end TryGetRange; "
        "  public static method Run(flag: bool) "
        "    if flag and TryGetRange(out var r) then _ := r.Start; end; "
        "    _ := r.Start; "  // read after short-circuit path — OK for value type
        "  end Run; "
        "end C;");
    // Range is a value type with a default; no definite-assignment error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Confirm that string (a real reference type) still requires definite-assignment:
// reading after a short-circuit path must ERROR.
BEZEL_TEST(sema_da,
           inline_out_var_string_still_requires_da_spec_22_4_3462) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  static method TryGetString(out s: string): bool "
        "    s := \"hi\"; "
        "    return true; "
        "  end TryGetString; "
        "  public static method Run(flag: bool) "
        "    if flag and TryGetString(out var s) then _ := s.Length; end; "
        "    _ := s.Length; "  // read after short-circuit path — ERROR for ref type
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

} // namespace
