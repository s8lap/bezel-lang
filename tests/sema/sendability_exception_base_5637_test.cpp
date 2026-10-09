// fix #5637 / s30_14_0007 (§30.14 / §12.4, v0.330): a sealed class
// deriving from a builtin exception (§1) is not sendable — the inherited state
// of a compiler-known class is not stated as transfer-compatible, so the class
// FAILS the rule (§30.14 verbatim: "and fails it otherwise"). Since no builtin
// exception declares its inherited state transfer-compatible, any class with a
// builtin exception base is non-sendable.
//
// Rows:
//   R1  Fault: Exception — as a Domain.Spawn argument — rejected
//   R2  Plain: int — as a Domain.Spawn argument — accepted (not Exception)
//
// Authority: spec §30.14 rule 2 + stdlib §1 builtin exception table.

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndTypecheckGated;
using bezel::test::SemaResult;

// R1 — sealed class deriving from Exception is not sendable as Spawn argument.
BEZEL_TEST(sendability_exception_base_5637,
           sealed_class_deriving_from_exception_rejected_spec_30_14) {
    auto r = parseAndTypecheckGated(
        "module M; "
        "import System.Threading; "
        "public sealed class Fault: Exception "
        "  public Detail: string := \"disk\"; "
        "end Fault; "
        "class C "
        "  public static method Handle(owned f: Fault) end Handle; "
        "  public method Run() "
        "    using var t := Domain.Spawn(C.Handle, new Fault()); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// R2 — sealed class with only primitive fields (not Exception) is sendable.
BEZEL_TEST(sendability_exception_base_5637,
           sealed_class_with_primitive_field_accepted_spec_30_14) {
    auto r = parseAndTypecheckGated(
        "module M; "
        "import System.Threading; "
        "public sealed class Plain "
        "  public Value: int := 0; "
        "end Plain; "
        "class C "
        "  public static method Handle(owned f: Plain) end Handle; "
        "  public method Run() "
        "    using var t := Domain.Spawn(C.Handle, new Plain()); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
