// fix #2827 — unit-type defects outside the statement position, the four-cell
// sweep swept out of #2819 (codegen / sema, spec §4.7 *The unit type*, §8.6
// *Boxing of the unit value*, §17.2 (the `(if …)` atom), §14.2 (unit satisfies
// `value` / `new()`), §A.4, v0.282). The issue bundles four pre-existing
// defects — all reproduced mechanically on 99ae9a4, three as compiler failures
// and one as a degradation note.
//
// What THIS test pins: the ONE remaining defect still live in the codegen as
// of d34c8b4 (defect 3, `Holder<()>` malformed IR — GEP into unsized type
// aborts the IR verifier), plus three regression guards covering the shapes
// the issue's own text names as "already fixed" so a future change cannot
// silently re-break them. The `(if …)`-with-unit-branches defect (#2) is
// still live on d34c8b4 but is the same family (#2915 / #3105) and would
// belong in a follow-up lane — not bundled here, so the test does not try to
// prove a FAIL-WITHOUT it cannot deliver.
//
// SPEC CITATIONS. §14.2: `where T: value` admits "the unit type" — `Holder<()>`
// is therefore GRAMMATICAL (§A.4 admits the unit type in a parameter / type-
// argument position) and the implementation must either lay the field out or
// document its rejection. §4.7: the unit type has exactly one value, so its
// representation carries no information; one inert byte is a faithful slot.
// The substitution the fix installs — `unitPlaceholderTy()` for a `Kind::Unit`
// field type — is the SAME swap `lowerLocalDecl` (#1619) and
// `lowerParamType`/`lowerParamTypeRef` (#1619 / #3105) already make at the
// local-decl and parameter-signature positions; the field arm joins the family.
//
// MECHANICAL PROOF (FAIL-WITHOUT / PASS-WITH). Reverting
// `src/codegen/LlvmBackend_Layout.cpp`'s `storageType` to its pre-fix form
// (the two `lowerType(...)` calls without the void→`unitPlaceholderTy()`
// substitution) and rebuilding produces, on the `Holder<()>` cell:
//
//   GEP into unsized type!
//     %3 = getelementptr inbounds nuw %"class.Holder$spec0", ptr %2, i32 0, i32 0
//   GEP into unsized type!
//     %disposed.flag = getelementptr inbounds nuw %"class.Holder$spec0",
//                      ptr %0, i32 0, i32 1
//   bezelc: codegen error: IR verification failed (malformed IR emitted; …)
//   rc=1
//
// restoring the substitution gives rc=0 with no diagnostic. The
// `var u: () := ();` cell is a regression guard for the same shape #2915
// covers — both stayed silent on d34c8b4 and would fail the same way if
// anyone removed the unit-init guard from `lowerLocalDecl`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Was this JitResult a SKIP (toolchain unavailable) rather than a real failure?
// Same helper the other runAot call sites carry.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// =====================================================================
// #2827 / §4.7 — `var u: () := ();` is the one position #1619 explicitly
// sanctions, and the unit-init guard in `lowerLocalDecl` must keep it
// silent on d34c8b4 (a regression guard; pre-#2915 it emitted the
// `#2434` `internal:` note).
// =====================================================================

BEZEL_TEST(codegen_unit_outside_stmt_2827,
           unit_local_decl_init_silent_spec_4_7) {
    JitResult r = runAot(prog("var u: () := (); return 51;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 51);
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("#2434") == std::string::npos);
    }
}

// =====================================================================
// #2827 / §14.2 — `Holder<()>` is the live defect; pre-fix the GEP into
// the unit-typed field aborts the IR verifier with "GEP into unsized
// type!" (rc=1). Post-fix the field is laid out as one `i8` (the §4.7
// placeholder, faithful because `()` has exactly one value), the
// specialization compiles, and the local's scope-end disposal runs.
// This is the cell that fails without the fix — the FAIL-WITHOUT proof.
// =====================================================================

BEZEL_TEST(codegen_unit_outside_stmt_2827,
           unit_as_generic_type_argument_does_not_malform_ir_spec_14_2_4_7) {
    JitResult r = runAot(
        "module M; "
        "public class Holder<T> public v: T; end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder<()>(); "
        "    return 52; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 52);
}

// Negative control on the same fix: a NON-unit type argument keeps
// compiling and the field read returns the held value (the field type
// went through the SAME `storageType` arm, so the substitution must be
// a no-op when the result is not `void`).
BEZEL_TEST(codegen_unit_outside_stmt_2827,
           non_unit_generic_field_unaffected_spec_14_2) {
    JitResult r = runAot(
        "module M; "
        "public class Holder<T> public v: T; end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder<int>() ; "
        "    h.v := 99; "
        "    return 53 + h.v - 99; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 53);
}

// =====================================================================
// #2827 / §28.10 — `using var u: () := ();` is the SAME `#2827` shape
// the issue text names. §28.10 emits a `vacuous` diagnostic on the
// path the BEZEL driver prints to stderr; the AOT harness captures
// only COMPILE-FAILURE diagnostics into `r.diagnostics`, so the vacuous
// note (Warning-severity) is NOT visible there. The regression guard
// is the compile EXIT (rc=0) and the program EXIT (rc=54): a regression
// that broke either would surface here as `r.ran == false` or a wrong
// exit code.
// =====================================================================

BEZEL_TEST(codegen_unit_outside_stmt_2827,
           using_var_unit_decl_compiles_spec_28_10) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var u: () := (); "
        "    return 54; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 54);
}

// =====================================================================
// #2827 / §21.9 — control: a method `Foo` returning `()` and CALLED FOR
// EFFECT (statement position, no value-demanding consumer) is the shape
// the issue lists as "fine and need no work" — it stays silent and the
// program returns the right answer. A regression to defects 1 (call-
// argument) or 4 (the previous `#2434` note on `var u: () := ();`) would
// show up as a failing compile here too.
// =====================================================================

BEZEL_TEST(codegen_unit_outside_stmt_2827,
           discarded_unit_call_stays_silent_spec_21_9) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Nop(): () end Nop; "
        "  public static method Main(): int "
        "    C.Nop(); "
        "    C.Nop(); "
        "    return 55; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("#2434") == std::string::npos);
}
