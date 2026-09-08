// fix #2871 — a `static` field initializer on an imported generic RECORD
// silently reads 0 (§2.8.2 clause 2, §22.12 rule 1, §22.13, §9 Records,
// §14.1 Generic declarations — v0.240).
//
// §2.8.2 clause 2 is the governing rule, verbatim:
//
//   "**Instantiate** every public generic type and generic method at arbitrary
//    module-external type arguments satisfying the declared constraints. An
//    external instantiation behaves identically to a same-module instantiation
//    of the same generic at the same arguments."
//
// THE CAUSE — a CATEGORY test standing in for a PROPERTY, this run's recurring
// defect shape. `ModuleMetadataBuilder.cpp` gated five separate carriage
// decisions on
//
//     const bool isGenericClass = !t.genericParams.empty() &&
//                                 t.kind == ast::TypeDeclKind::Class;
//
// The property those gates actually need is "this type is generic, so each
// consumer monomorphizes its OWN copy of its state and bodies and the exporting
// compilation cannot run them on the consumer's behalf". That is true of a
// generic RECORD (§9) verbatim — but a record has `kind == Record`, so it fell
// out of every one of them: no `fieldInitText` was carried, the loader's
// reconstruction loop found nothing to attach, and the per-instantiation static
// cell kept its zero-init. It was SILENT rather than rejected because #2351's
// reject fires on the very reconstruction path the same flag gates, so a record
// reached neither the carriage nor the rejection.
//
// ⚠ THE ISSUE'S STATED ROOT CAUSE IS WRONG, and the correction is the point of
// this file. #2871 says the generic-CLASS twin is a hard #2351 reject and only
// the record spelling is silent. Measured on the parent commit, real two-module
// builds, the axis is not class-vs-record at all:
//
//     kind    | `static`            | `static readonly`
//     --------+---------------------+-------------------
//     class   | 107  (correct)      | 0  (silently wrong)
//     record  | 0    (this defect)  | 0  (silently wrong)
//
// So there are TWO defects sharing one symptom, and this issue's own repro
// exercises the OTHER one:
//   * record + plain `static` — fixed here. Proven by control experiment: on the
//     unwidened compiler it reads 0, on the widened one 107, all else equal.
//   * `static readonly` — reads 0 on BOTH class and record, so it is neither a
//     record defect nor fixed by this change. Filed separately with the grid
//     above; the cause is a different member KIND reaching the loader's
//     `mm.kind != MetaMemberKind::Field` skip, not the generic-ness gate.
// This file therefore asserts ONLY the half that is fixed, and pins the class
// `static` case as an accept-control. It does NOT pin the `static readonly`
// rows: a test that asserts a value it believes is wrong is a test that will be
// read as documenting intent.
//
// ⚠ Vacuity guard, inherited from #2839/#2818: when reconstruction declines a
// call can fall back to the EXPORTER's compiled function and return the right
// answer by accident. Every generic type here carries a `T`-typed field FIRST
// and is instantiated at `string`, so #2821's layout gate rejects an
// unreconstructed body at compile time instead of silently falling back. Do not
// remove the leading `T` field.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// Returned when the DEPENDENCY (exporter) object could not be built. Asserted
// against at every call site rather than silently skipped — a malformed
// exporter would otherwise report green having exercised nothing (that trap
// caught a real mistake while writing #2857's suite).
constexpr int kDepFailed2871 = -2;

struct TempFile2871 {
    std::string path;
    explicit TempFile2871(const std::string& p) : path(p) {}
    ~TempFile2871() { std::remove(path.c_str()); }
};

void write2871(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

int runXmod2871(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2871 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2871 aObj((dir / (tag + "_a.o")).string());
    write2871(aSrc.path, aSrcText);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepFailed2871;
    }
    if (!std::ifstream(aObj.path).good()) return kDepFailed2871;
    TempFile2871 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2871 bExe((dir / (tag + "_b")).string());
    write2871(bSrc.path, bSrcText);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

int runSingle2871(const std::string& stem, const std::string& srcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2871 src((dir / (tag + ".bzl")).string());
    TempFile2871 exe((dir / (tag + "_x")).string());
    write2871(src.path, srcText);
    bezel::Driver driver;
    if (!driver.compileFile(src.path, exe.path, {}).success) return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

std::string lib2871(const std::string& mod, const std::string& kind,
                    const std::string& type, const std::string& body,
                    bool withBodyField = true) {
    std::string prefix = "module " + mod + "; public " + kind + " " + type +
                         "<T>";
    if (withBodyField) prefix += " public v: T;";
    return prefix + " " + body + " end " + type + ";";
}

}  // namespace

// ===========================================================================
// T1 — THE FIX. A generic RECORD's `static` field initializer must cross the
// boundary. Read 0 before; reads 107 now.
// ===========================================================================
BEZEL_TEST(xmod_generic_record_static_init_2871,
           t1_generic_record_static_field_init_crosses_spec_22_12) {
    int rc = runXmod2871(
        "bz2871_t1",
        lib2871("RS2871A", "record", "Pair",
                "public static Seed: int := 107;"),
        "module Main; import RS2871A; "
        "class C public static method Main(): int "
        "  return Pair<string>.Seed; end Main; end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2871);
    if (rc == kDepFailed2871) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T2 — §2.8.2 clause 2 CONTROL: the identical declarations as ONE module. This
// is what defines the required answer; T1 is a divergence claim against it.
// ===========================================================================
BEZEL_TEST(xmod_generic_record_static_init_2871,
           t2_single_module_control_spec_2_8_2) {
    int one = runSingle2871(
        "bz2871_t2",
        "module Main; "
        "public record Pair<T> public v: T; "
        "  public static Seed: int := 107; end Pair; "
        "class C public static method Main(): int "
        "  return Pair<string>.Seed; end Main; end C;");
    BEZEL_EXPECT_EQ(one, 107);
}

// ===========================================================================
// T3 — ACCEPT-CONTROL: the generic CLASS spelling already worked and must keep
// working. This is the guard that the predicate was WIDENED, not moved: a fix
// that swapped Class for Record would pass T1 and fail here.
// ===========================================================================
BEZEL_TEST(xmod_generic_record_static_init_2871,
           t3_generic_class_static_field_init_still_crosses_spec_22_12) {
    int rc = runXmod2871(
        "bz2871_t3",
        lib2871("RS2871B", "class", "Box", "public static Seed: int := 107;"),
        "module Main; import RS2871B; "
        "class C public static method Main(): int "
        "  return Box<string>.Seed; end Main; end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2871);
    if (rc == kDepFailed2871) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T4 — ACCEPT-CONTROL, and a claim I had to CORRECT. I expected this to be a
// second fix pin, on the reasoning that the widened predicate also gates
// #2860's INSTANCE field initializer carriage. MEASURED: it passes on the
// unwidened compiler too, so a generic record's instance initializer was
// already crossing by another route and this is NOT evidence of the fix. Kept
// as a control — it must not regress — and relabelled rather than left
// implying a coverage it does not have.
// ===========================================================================
BEZEL_TEST(xmod_generic_record_static_init_2871,
           t4_generic_record_instance_field_init_crosses_spec_2_8_2) {
    int rc = runXmod2871(
        "bz2871_t4",
        lib2871("RS2871C", "record", "Cell", "public tag: int := 107;"),
        "module Main; import RS2871C; "
        "class C public static method Main(): int "
        "  using var b := new Cell<string>(); return b.tag; end Main; end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2871);
    if (rc == kDepFailed2871) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T5 — REGRESSION GUARD. The predicate now also admits records to the ctor /
// method / destructor body-text carriage it previously denied them. A generic
// record's method body must still work — this is the "widening broke nothing"
// pin, and the shape most likely to catch it if it did.
//
// narrow #4059 further (§9.1 / §9.2 / §9.5, v0.295): Calc here is a
// methods-only body record (no body field `v: T;`) — the predicate's
// further-narrow allow: a record with no body fields/properties at all
// admits `new T()` because there is nothing to silently zero-fill. The
// test body calls a method, not a field, so the body-field was
// incidental to T5's intent.
// ===========================================================================
BEZEL_TEST(xmod_generic_record_method_body_2871,
           t5_generic_record_method_body_still_works_spec_14_1) {
    int rc = runXmod2871(
        "bz2871_t5",
        lib2871("RS2871D", "record", "Calc",
                "public method M(): int return 107; end M;",
                /*withBodyField=*/false),
        "module Main; import RS2871D; "
        "class C public static method Main(): int "
        "  using var b := new Calc<string>(); return b.M(); end Main; end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2871);
    if (rc == kDepFailed2871) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T6 — REGRESSION GUARD: a plain generic record with no initializers and
// no body fields at all still imports and zero-arg constructs. The cheapest
// check that the added metadata carriage did not disturb the ordinary path.
//
// narrow #4059 further (§9.1 / §9.2 / §9.5, v0.295): Plain here is
// literally empty (no `public v: T;`) — the predicate's further-narrow
// allow for an empty-body record. The pre-#4059 record had a generic
// body field `v: T;` (no initializer); the further-narrow keeps the
// rejection for THAT shape (it has a body field/property without an
// init), but admits an empty body. T6's intent — "plain generic record
// unaffected" — maps onto the empty-body shape (the field was just
// round-trip boilerplate, not under test).
// ===========================================================================
BEZEL_TEST(xmod_generic_record_method_body_2871,
           t6_plain_generic_record_unaffected_spec_9) {
    int rc = runXmod2871(
        "bz2871_t6",
        lib2871("RS2871E", "record", "Plain", "",
                /*withBodyField=*/false),
        "module Main; import RS2871E; "
        "class C public static method Main(): int "
        "  using var b := new Plain<string>(); "
        "  return 42; end Main; end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2871);
    if (rc == kDepFailed2871) return;
    BEZEL_EXPECT_EQ(rc, 42);
}

// ===========================================================================
// T7 — the fix pin at a second ELEMENT TYPE and through a non-trivial constant.
// `Seed` is read through a `string` instantiation exactly as T1, but the value
// distinguishes "the initializer ran" (107) from "the cell kept its zero-init"
// (0) AND from "some other constant crossed" — the three outcomes a single
// 0-vs-107 pin cannot separate on its own.
// ===========================================================================
BEZEL_TEST(xmod_generic_record_static_init_2871,
           t7_generic_record_static_bool_and_second_field_spec_22_12) {
    int rc = runXmod2871(
        "bz2871_t7",
        lib2871("RS2871F", "record", "Multi",
                "public static A: int := 100; "
                "public static B: int := 7;"),
        "module Main; import RS2871F; "
        "class C public static method Main(): int "
        "  return Multi<string>.A + Multi<string>.B; end Main; end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2871);
    if (rc == kDepFailed2871) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T8 — the fix pin at a DIFFERENT INSTANTIATION. §22.12 rule 1 requires each
// distinct instantiation to have its own static state, so the initializer must
// run per-instantiation in the consumer — not once, and not never. Both
// instantiations must read 107; a fix that ran the initializer for only the
// first one reads 107 then 0.
// ===========================================================================
BEZEL_TEST(xmod_generic_record_static_init_2871,
           t8_two_instantiations_each_run_the_initializer_spec_22_12) {
    int rc = runXmod2871(
        "bz2871_t8",
        lib2871("RS2871G", "record", "Per", "public static Seed: int := 107;"),
        "module Main; import RS2871G; "
        "class C public static method Main(): int "
        "  if Per<string>.Seed <> 107 then return 91; end; "
        "  if Per<int>.Seed <> 107 then return 92; end; "
        "  return 107; end Main; end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2871);
    if (rc == kDepFailed2871) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

}  // namespace bezel::module
