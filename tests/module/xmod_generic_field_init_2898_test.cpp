// Cross-module — #2898 (§17.14 / §2.8.2 clause 2 / §22.13, v0.240): a folded
// compile-time constant must cross as an imported GENERIC class's instance
// field initializer.
//
// `renderConstExpr` (src/module/ModuleMetadataBuilder.cpp) had arms only for
// BARE literals, so a §17.14 compile-time constant written `:= 100 + 7`
// rendered "" and every consumer read that as "not a compile-time constant".
// For an imported generic's instance field that is a false REJECT: #2860's
// guard is category-blind (it asks whether a value reached the slot), so an
// empty rendering made it refuse a construction §2.8.2 clause 2 requires to
// behave exactly as the same-module one does — "An external instantiation
// behaves identically to a same-module instantiation of the same generic at the
// same arguments."
//
// The fix FOLDS such an expression to a literal, through the one §17.14 folder
// (`sema::foldIntConst`), rather than rendering it textually — `valueText` is
// consumed as TEXT by codegen (`constValues_`, ModuleLoader.cpp), so widening
// what it may contain would make every consumer an evaluator. Folding keeps it a
// literal, the shape they all already assume.
//
// ⚠ This fix DEPENDS on #2936 and had to land with it. #2898's fold runs through
// `sema::foldIntConst`, whose integer-literal arm was a base-10 `std::stoll`
// until #2936. MEASURED: with this fold present and #2936 reverted,
// `:= 0x60 + 11` crosses as **11** — a silent wrong value where a loud rejection
// stood before. That composition is the last case in this file.

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
// run the result, returning the process exit code (-1 if any step failed — which
// is how the #2898 false reject registers). Exit codes are 8-bit, so every
// expected value below is kept under 256.
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
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// --- #2898: a folded §17.14 constant crosses as a generic's field init ------

// The headline: `:= 100 + 7` on an imported GENERIC class's instance field. The
// #2860 guard rejected the CONSTRUCTION because `renderConstExpr` returned "".
// -1 here means the compile failed, i.e. the false reject is back.
BEZEL_TEST(xmod_generic_field_init_2898, generic_field_init_constant_expression_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_add",
            "module BLib; public class Box<T> "
            "  public tag: int := 100 + 7; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Box;",
            "module BApp; import BLib; class C "
            "  public static method Main(): int "
            "    using var b := new Box<int>(1); return b.tag; "
            "  end Main; end C;"),
        107);
}

// CONTROL: the bare-literal spelling already crossed (that is what #2860
// fixed), so this passes both ways. It is the reference the subject row must
// match — §2.8.2 clause 2 is precisely "the external instantiation behaves
// identically", so `:= 107` and `:= 100 + 7` must agree.
BEZEL_TEST(xmod_generic_field_init_2898, generic_field_init_bare_literal_control_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_lit",
            "module LLib; public class Lit<T> "
            "  public tag: int := 107; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Lit;",
            "module LApp; import LLib; class C "
            "  public static method Main(): int "
            "    using var b := new Lit<int>(1); return b.tag; "
            "  end Main; end C;"),
        107);
}

// Operator coverage for the fold: subtraction, multiplication+addition,
// parenthesised division, shift+or, double negation, and `mod` — every one
// written to reach the same 107 so a wrong fold is a wrong exit code rather
// than a compile failure.
BEZEL_TEST(xmod_generic_field_init_2898, generic_field_init_operator_grid_spec_17_14) {
    const char* exprs[] = {"200 - 93", "10 * 10 + 7", "(100 + 14) / 2 + 50",
                           "1 << 6 | 43", "-(-107)", "106 mod 100 + 101"};
    for (const char* e : exprs) {
        std::string lib =
            std::string("module GLib; public class Ar<T> "
                        "  public tag: int := ") + e +
            "; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Ar;";
        BEZEL_EXPECT_EQ(
            buildAndRun("bezel_2898_grid",
                lib,
                "module GApp; import GLib; class C "
                "  public static method Main(): int "
                "    using var b := new Ar<int>(1); return b.tag; "
                "  end Main; end C;"),
            107);
    }
}

// ⚠ THE COMPOSITION, and the reason #2936 had to land WITH #2898: a hex operand
// inside the folded initializer. With #2898 alone, `sema::foldIntConst`'s
// base-10 literal arm read `0x60` as 0, so this would have crossed as 11 — a
// silent wrong value where a loud rejection stood before. Both fixes present:
// 107.
BEZEL_TEST(xmod_generic_field_init_2898, hex_operand_in_generic_field_initializer_spec_a8) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_hex",
            "module XLib; public class Hx<T> "
            "  public tag: int := 0x60 + 11; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Hx;",
            "module XApp; import XLib; class C "
            "  public static method Main(): int "
            "    using var b := new Hx<int>(1); return b.tag; "
            "  end Main; end C;"),
        107);
}

// --- OVER-ACCEPTANCE controls: what must STILL be rejected ------------------
//
// #2898 widens what crosses the artifact boundary, so the guard it relaxes must
// be shown still to fire. #2874 widens further: it CARRIES everything the
// load-time parse-and-attach can reconstruct. A literal-arithmetic call on a
// string (`100 + "abcdefg".Length` — sema resolves `"abcdefg".Length` to `int`
// and `100 + int` to `int`, the slot gets the value) and a float-arithmetic
// expression (`1.5 + 0.5` — sema + lowerExpr produces f64 2.0) are BOTH
// carriable post-#2874, so the pre-#2874 "still rejected" shapes are no
// longer the right over-acceptance controls. The new controls must exercise
// a shape that genuinely fails to reconstruct across the module boundary:
// a reference to an INTERNAL helper class — visible to the LIBRARY's own
// compile (the field initializer text is exported verbatim) but absent from
// the metadata (only public types are exported, §13.2), so the importer's
// synthetic-source resolveTypes fails and the §2.8.2 construction-site
// diagnostic fires.

// An initializer that calls a method on an INTERNAL helper class: the helper
// is in the library's own scope but NOT in the metadata, so the importer's
// synthetic source cannot resolve the call. §2.8.2 "must reject with a
// diagnostic naming the shape" still applies — the diagnostic fires at the
// construction site, exactly as the pre-#2874 false-reject did. The test
// pins that the load-time parse did NOT silently widen to "carry anything";
// genuine visibility gaps remain a loud reject.
BEZEL_TEST(xmod_generic_field_init_2898, non_constant_initializer_still_rejected_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_rej",
            "module SLib; "
            "internal class Internal2898 "
            "  public static method make(): int => 100; "
            "end Internal2898; "
            "public class St<T> "
            "  public tag: int := Internal2898.make(); public v: T; "
            "  public constructor(x: T) v := x; end constructor; end St;",
            "module SApp; import SLib; class C "
            "  public static method Main(): int "
            "    using var b := new St<int>(1); return b.tag; "
            "  end Main; end C;"),
        -1);
}

// A FLOAT constant expression: post-#2874 this RUNS (the parse succeeds, the
// lowerExpr emits `fadd` and the slot stores 2.0), so the pre-#2874
// over-acceptance assertion no longer holds — the new over-acceptance is the
// same internal-helper reference (above), exercised on a `double` slot so a
// reader of this file cannot mistakenly conclude "post-#2874 float is
// rejected". What remains rejected is the visibility-gap shape, regardless
// of the slot type.
BEZEL_TEST(xmod_generic_field_init_2898, float_constant_initializer_still_rejected_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_dbl",
            "module DLib; "
            "internal class InternalD2898 "
            "  public static method make(): double => 1.5; "
            "end InternalD2898; "
            "public class Db<T> "
            "  public tag: double := InternalD2898.make() + 0.5; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Db;",
            "module DApp; import DLib; class C "
            "  public static method Main(): int "
            "    using var b := new Db<int>(1); return 0; "
            "  end Main; end C;"),
        -1);
}

}  // namespace bezel::module
