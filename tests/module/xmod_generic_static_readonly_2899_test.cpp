// Cross-module — #2899 (§22.12 rule 1 / §22.13 / §2.8.2 clause 2, v0.240): a
// `static readonly` field of an imported GENERIC type whose initializer is a
// bare constant LITERAL must read its declared value, not its §7.9 default.
//
// §22.12: "A static member of a *generic* class is accessed by supplying the
// type arguments on the type name in receiver position — `Registry<int>.
// Register()`, `Cache<string>.Default`. Each distinct instantiation has its own
// static state (§22.13)." — and, of the two spellings this file separates,
// "`static readonly` accepts any value but is read at runtime."
// §2.8.2 clause 2: "**Instantiate** every public generic type and generic method
// at arbitrary module-external type arguments satisfying the declared
// constraints. An external instantiation behaves identically to a same-module
// instantiation of the same generic at the same arguments."
//
// ROOT CAUSE. A `readonly` static's constant-literal initializer is BAKED into
// its backing global (`isBakedStaticFieldInit`, #1309) and `genericInstSfinitFn`
// therefore skips its runtime store. `getOrCreateGenericInstStaticGlobal` builds
// the per-instantiation global by COPYING the template-level global's
// initializer — but an IMPORTED generic's template-level global is an external
// DECLARATION with no initializer (declareStatics), so the copy fell back to
// zero-init. Both halves declined and the value was simply gone. The plain
// `static` (mutable) spelling was never affected: it is not baked, so the
// sfinit store carries it. That is the real axis — `static` vs `static
// readonly` — not the record-vs-class one #2871 named; a generic RECORD and a
// generic CLASS measured identically here, before and after.
//
// The fix restores the bake on the imported side rather than dropping the
// sfinit skip, because the per-instantiation global and the sfinit function are
// SEPARATELY-merged LinkOnceODR symbols: a program in which the declaring module
// also instantiates the type can link one module's global against the other
// module's sfinit, and only "every module's definition of the global carries the
// value" is correct under every pairing (exporter_also_instantiates_... below).

#include "module/ModuleLoader.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a library object, then compile+link `appSrc` against it and
// run the result. Returns the process exit code; -1 if any compile step failed
// (which is how a §2.8.2 rejection registers) and -2 if the program died on a
// SIGNAL rather than exiting — the string row below needs that distinction,
// because before the fix it read the null watermark and SIGSEGV'd rather than
// returning a wrong number. Exit codes are 8-bit, so every expected value here
// is kept under 256.
int buildAndRun(const std::string& stem, const std::string& libSrc,
                const std::string& appSrc) {
    auto dir = std::filesystem::temp_directory_path();
    TempFile a((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile b((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(a.path, libSrc);
    writeSource(b.path, appSrc);

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(a.path, aObj.path);
    if (!ra.success) return -1;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(b.path, bExe.path, deps);
    if (!rb.success) return -1;
    int status = std::system(bExe.path.c_str());
    if (status == -1) return -1;
    if (WIFSIGNALED(status)) return -2;   // died on a signal (the pre-fix string row)
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The same declarations compiled as ONE module — the §2.8.2 clause 2 reference
// every cross-module row must match. Kept separate from buildAndRun so a row
// can state both halves and neither can be mistaken for the other.
int buildAndRunSingle(const std::string& stem, const std::string& src) {
    auto dir = std::filesystem::temp_directory_path();
    TempFile s((dir / (stem + "_s.bzl")).string());
    TempFile exe((dir / (stem + "_s.out")).string());
    writeSource(s.path, src);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(s.path, exe.path, {});
    if (!r.success) return -1;
    int status = std::system(exe.path.c_str());
    if (status == -1) return -1;
    if (WIFSIGNALED(status)) return -2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// --- #2899: the value must cross ------------------------------------------

// The headline, generic CLASS. Before the fix: 0, with no diagnostic at any
// severity. The single-module control is asserted in the same test so the row
// states §2.8.2 clause 2's "behaves identically" as one comparison.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           readonly_literal_static_on_imported_generic_class_spec_22_12) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_cls",
            "module RLibA; public class Box<T> "
            "  public v: T; public static readonly Seed: int := 107; end Box;",
            "module RAppA; import RLibA; class C "
            "  public static method Main(): int return Box<string>.Seed; "
            "  end Main; end C;"),
        107);
    BEZEL_EXPECT_EQ(
        buildAndRunSingle("bezel_2899_cls",
            "module Main; public class Box<T> "
            "  public v: T; public static readonly Seed: int := 107; end Box; "
            "class C public static method Main(): int return Box<string>.Seed; "
            "  end Main; end C;"),
        107);
}

// The same shape on a generic RECORD. #2871 named record-vs-class as the axis;
// this row and the class row above measure IDENTICALLY, before and after, which
// is what demotes that axis and promotes `static` vs `static readonly`.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           readonly_literal_static_on_imported_generic_record_spec_22_12) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_rec",
            "module RLibB; public record Pair<T> "
            "  public v: T; public static readonly Seed: int := 107; end Pair;",
            "module RAppB; import RLibB; class C "
            "  public static method Main(): int return Pair<string>.Seed; "
            "  end Main; end C;"),
        107);
}

// The MEMORY-SAFETY row, and the reason #2899 is not merely a wrong int: a
// `static readonly` of type `string` is §22.12 rule 3's unrestricted
// shared-immutable spelling, and it read the §7.9 null watermark — §22.13's own
// text for that state is that it "raises `NullReferenceException` at the read in
// checked disposal mode ... and is undefined behavior to use unchecked". Before
// the fix this row SIGSEGV'd (-2 here), so the oracle is a real content
// comparison, not just a length: a wrong 7-character string would pass a
// length-only check.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           readonly_string_static_on_imported_generic_spec_22_13) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_str",
            "module RLibC; public class Bs<T> "
            "  public v: T; public static readonly S: string := \"abcdefg\"; "
            "end Bs;",
            "module RAppC; import RLibC; class C "
            "  public static method Main(): int "
            "    if Bs<string>.S = \"abcdefg\" then "
            "      return 100 + Bs<string>.S.Length; end if; "
            "    return 1; "
            "  end Main; end C;"),
        107);
}

// The field-TYPE axis: bool, double and long all lost their value identically,
// so the defect is not int-specific and not tied to one LLVM constant shape.
// Every row returns 107 on success and a DIFFERENT non-zero on failure, so a
// degraded zero can never satisfy one.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           readonly_static_field_type_grid_spec_22_12) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_bool",
            "module RLibD; public class Bb<T> "
            "  public v: T; public static readonly S: bool := true; end Bb;",
            "module RAppD; import RLibD; class C "
            "  public static method Main(): int "
            "    if Bb<string>.S then return 107; end if; return 1; "
            "  end Main; end C;"),
        107);
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_dbl",
            "module RLibE; public class Bd<T> "
            "  public v: T; public static readonly S: double := 107.5; end Bd;",
            "module RAppE; import RLibE; class C "
            "  public static method Main(): int "
            "    if Bd<string>.S > 100.0 then return 107; end if; return 2; "
            "  end Main; end C;"),
        107);
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_long",
            "module RLibF; public class Bl<T> "
            "  public v: T; public static readonly S: long := 107; end Bl;",
            "module RAppF; import RLibF; class C "
            "  public static method Main(): int "
            "    if Bl<string>.S = 107 then return 107; end if; return 3; "
            "  end Main; end C;"),
        107);
}

// §22.12: "Each distinct instantiation has its own static state." Both
// instantiations must carry the value, so the oracle SUMS them and subtracts one
// copy — 107+107-107 = 107 when both are right, and 149 (an 8-bit -107) when
// both are zero. Deliberately NOT `A.S - B.S + 107`, which is the `x = x` shape
// that also holds when both read zero.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           each_instantiation_carries_the_readonly_value_spec_22_12) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_two",
            "module RLibG; public class Bt<T> "
            "  public v: T; public static readonly S: int := 107; end Bt;",
            "module RAppG; import RLibG; class C "
            "  public static method Main(): int "
            "    return Bt<string>.S + Bt<int>.S - 107; "
            "  end Main; end C;"),
        107);
}

// The LINKAGE row this fix's shape is chosen for. Here the DECLARING module
// also instantiates `Bm<string>`, so both objects define the LinkOnceODR
// per-instantiation global `bz$...$S` AND the LinkOnceODR `bz$...$sfinit` — and
// the linker merges the two symbols independently, so the surviving pair may
// come from different modules. Baking the value into every module's definition
// of the GLOBAL is correct under every pairing; making the sfinit store instead
// would leave {importer's zero global, exporter's store-free sfinit} reading
// zero. Both the exporter-side read and the importer-side read are checked.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           exporter_also_instantiates_the_same_generic_spec_2_8_3) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_merge",
            "module RLibH; public class Bm<T> "
            "  public v: T; public static readonly S: int := 107; end Bm; "
            "public class Helper public static method Get(): int "
            "  return Bm<string>.S; end Get; end Helper;",
            "module RAppH; import RLibH; class C "
            "  public static method Main(): int "
            "    return Bm<string>.S + Helper.Get() - 107; "
            "  end Main; end C;"),
        107);
}

// --- CONTROLS ---------------------------------------------------------------

// CONTROL, passes both ways: the plain `static` (MUTABLE) spelling was always
// correct — it is not baked, so `genericInstSfinitFn`'s runtime store carries
// it. This is the row that names the axis: identical declaration, identical
// read, one modifier apart, and only the `readonly` one was broken.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           mutable_static_control_spec_22_12) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_mut",
            "module RLibI; public class Mi<T> "
            "  public v: T; public static Mut: int := 107; end Mi;",
            "module RAppI; import RLibI; class C "
            "  public static method Main(): int return Mi<string>.Mut; "
            "  end Main; end C;"),
        107);
}

// CONTROL, passes both ways: a `readonly` static whose initializer is NOT a bare
// literal is not baked, so it always went through the sfinit store and always
// worked. Pins that the fix did not have to touch — and did not disturb — that
// path.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           readonly_non_literal_initializer_control_spec_22_13) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_expr",
            "module RLibJ; public class Bx<T> "
            "  public v: T; public static readonly S: int := 100 + 7; end Bx;",
            "module RAppJ; import RLibJ; class C "
            "  public static method Main(): int return Bx<string>.S; "
            "  end Main; end C;"),
        107);
}

// OVER-ACCEPTANCE CONTROL. This fix widens what an imported generic's static
// field may carry, so #2351's guarantee — never silently observe an
// uninitialized value — must be shown still to fire. An initializer referencing
// an `internal` sibling genuinely cannot be reconstructed across the boundary:
// §2.8.2 clause 1 requires the interface to type-check uses of "public (and, for
// cross-module derivation, `protected`) types and members", and an `internal`
// top-level type (§13.2) is neither. §2.8.2's "No restrictions beyond
// visibility" then requires an implementation "unable to carry
// some body shape in its interface format" to "reject the export, or the import,
// with a diagnostic naming the shape; it must not alter the instantiation's
// semantics". -1 is that rejection. Written with the `readonly` spelling
// precisely because that is the one this fix changes.
BEZEL_TEST(xmod_generic_static_readonly_2899,
           unreconstructible_readonly_static_still_rejected_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_rej",
            "module RLibK; internal class Hidden "
            "  public static method Val(): int return 107; end Val; end Hidden; "
            "public class Bz<T> public v: T; "
            "  public static readonly S: int := Hidden.Val(); end Bz;",
            "module RAppK; import RLibK; class C "
            "  public static method Main(): int return Bz<string>.S; "
            "  end Main; end C;"),
        -1);
}

// BOUNDARY CONTROL, passes both ways. The fix lives entirely in the
// per-instantiation STATIC-storage path (`getOrCreateGenericInstStaticGlobal`).
// An imported generic's `readonly` INSTANCE field is a different mechanism —
// #2860's construction-site replay of `valueText`, extended by #2898 — and must
// be untouched. Sharing the `readonly` modifier with the rows above, it is the
// row that shows the fix did not widen along that modifier into instance
// storage.
//
// (The mutable-vs-readonly gate's NEGATIVE direction is deliberately not
// asserted here: a mutable static's literal is stored at runtime by
// `genericInstSfinitFn` and a `readonly` one is baked into the global, but both
// spellings produce the SAME observable value from Bezel, so any test claiming
// to separate them would be pinning something it cannot see. The two rows that
// DO differ — mutable_static_control_spec_22_12 above, correct before and after,
// against the readonly rows, wrong before and correct after — are what name the
// axis.)
BEZEL_TEST(xmod_generic_static_readonly_2899,
           readonly_instance_field_boundary_control_spec_22_8) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2899_inst",
            "module RLibM; public class Bi<T> public v: T; "
            "  public readonly tag: int := 107; "
            "  public constructor(x: T) v := x; end constructor; end Bi;",
            "module RAppM; import RLibM; class C "
            "  public static method Main(): int "
            "    using var b := new Bi<int>(1); return b.tag; "
            "  end Main; end C;"),
        107);
}

}  // namespace bezel::module
