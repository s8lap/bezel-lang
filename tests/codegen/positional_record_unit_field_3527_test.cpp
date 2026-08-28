// fix #3527 — DECLARING a positional record with a unit-typed field killed
// `bezelc` with SIGILL and no diagnostic (spec §4.7 *The unit type*, §9.1
// *Declaration forms*, §8.5 *`ToString`*, §8.6 *Boxing* — v0.283).
//
// MEASURED on ef339fa. No VALUE of the record is needed anywhere; the
// declaration alone is enough:
//
//     module MX2;
//     record R(A: int, U: ());
//     class C  public static method Main(): int
//       Console.WriteLine("no record value at all"); return 0; end Main; end C;
//
//   bezelc: rc=132 (SIGILL), stdout empty, stderr empty. Under gdb:
//
//     #0  llvm::StructLayout::StructLayout(llvm::StructType*, DataLayout const&)
//     #1  llvm::DataLayout::getABITypeAlign(llvm::Type*) const
//     #2  llvm::IRBuilderBase::CreateAlignedLoad         (IRBuilder.h:1883)
//     #5  Emitter::boxToStringThunk       (LlvmBackend_Setup.cpp:32179)
//     #6  Emitter::setupBoxDescriptors    (LlvmBackend_Setup.cpp:30806)
//
// `setupBoxDescriptors` mints a §8.6 box ToString thunk for EVERY record in the
// module — which is why no use of the record is required — and that thunk loads
// the record value. `recordStruct` had laid the record out with a `void` MEMBER,
// and LLVM's struct-layout walk executes an `llvm_unreachable` on one (a `ud2`
// in a release LLVM, hence a hardware trap rather than a message).
//
// ── ROOT CAUSE: ONE ARM OF #2827's OWN SUBSTITUTION WAS MISSING ──────────────
//
// §4.7: "`()` is the type with exactly one value". `lowerType` answers LLVM
// `void` for exactly that one type, and every STORAGE position substitutes the
// one inert byte instead. `Emitter::storageType` has three member arms; #2827
// gave the substitution to the body-`FieldDecl` arm and the auto-`PropertyDecl`
// arm and not to the POSITIONAL one — which is the arm a §9.1 positional record
// takes, its field type riding the originating `ast::Param` rather than a
// `FieldDecl`. The fix routes all three (and the alloca / parameter /
// state-machine-frame sites) through one `unitStorageTy`, so a missed arm is a
// missed CALL rather than a silently different spelling.
//
// ── WHY THESE ROWS ───────────────────────────────────────────────────────────
//
// Pre-fix every row fails at `r.ran` — no binary was produced. "It compiles" is
// therefore NOT the oracle: a fix that made the field zero-width or pointer-wide
// would also stop the trap and would shift or corrupt the fields around it. Each
// row reads the NEIGHBOURING components back, and T2 pins the §8.5 rendering
// across all three spellings of the same value, which is where a wrong field
// width shows up as text.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── T1 — THE MINIMAL REPRO: the record is DECLARED and never used. Pre-fix
// rc=132 with no diagnostic; post-fix the program runs and prints its marker.
BEZEL_TEST(codegen_positional_record_unit_field_3527,
           declaring_positional_record_with_unit_field_spec_4_7_9_1) {
    auto r = runAot(
        "module M3527A; "
        "record R(A: int, U: ()); "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(\"no record value at all\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("no record value at all\n"));
}

// ── T2 — THE LAYOUT ORACLE. The unit component sits BETWEEN two components that
// must read back unchanged, so a zero-width or over-wide slot shifts or corrupts
// them. §8.5, for a positional record, renders the field listing — and §8.5's
// unit rendering is `"()"`, the same constant every other unit position answers
// with. All three spellings of the one value (direct, boxed via `Object`, and
// §18.5 interpolation) are asserted to agree, which is what a wrong slot breaks.
BEZEL_TEST(codegen_positional_record_unit_field_3527,
           unit_component_does_not_disturb_its_neighbours_spec_9_1_8_5_8_6) {
    auto r = runAot(
        "module M3527B; "
        "record R(A: int, U: (), B: string); "
        "class C "
        "  public static method Main(): int "
        "    var r := new R(7, (), \"s\"); "
        "    Console.WriteLine(r.A.ToString()); "
        "    Console.WriteLine(r.B); "
        "    Console.WriteLine(r.U.ToString()); "
        "    Console.WriteLine(r.ToString()); "
        "    var o: Object := r; "
        "    Console.WriteLine(o.ToString()); "
        "    Console.WriteLine($\"interp {r}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("7\ns\n()\n"
                                "R(A: 7, U: (), B: s)\n"
                                "R(A: 7, U: (), B: s)\n"
                                "interp R(A: 7, U: (), B: s)\n"));
}

// ── T3 — the TRAILING position, so a fix that happened to work only when the
// unit component is followed by another one still fails. The record is also
// CONSTRUCTED and read here, which the box-descriptor pass alone does not do.
BEZEL_TEST(codegen_positional_record_unit_field_3527,
           trailing_unit_component_constructed_and_read_spec_9_1) {
    auto r = runAot(
        "module M3527C; "
        "record R(A: int, U: ()); "
        "class C "
        "  public static method Main(): int "
        "    var r := new R(1, ()); "
        "    Console.WriteLine(r.A.ToString()); "
        "    Console.WriteLine(r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nR(A: 1, U: ())\n"));
}
