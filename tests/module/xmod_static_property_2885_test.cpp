// fix #2885 (§22.12 "Static members" / §22.13 "Static initialization order" /
// §2.8.2 clause 1 / §23, v0.240): an IMPORTED STATIC property had NO
// cross-module accessor ABI at all — every spelling was silently wrong.
//
// MEASURED ON THE PARENT COMMIT (2aed89a), on real two-module builds
// (`bezelc -c lib.bzl`, then `bezelc --import lib.o app.bzl`), against a
// byte-identical SAME-MODULE control that answers correctly:
//
//   HS.SP := 7;   then HS._q   ->   0   (8 is correct: the setter stores value+1)
//   HS.SP += 1;   then HS._q   ->   0   (110 is correct)
//   HS.SP++;      then HS._q   ->   0   (212 is correct)
//   var r := HS.SP;            ->   0   (100 is correct) + an "internal: …
//                                        #2434" degrade diagnostic
//   "x" + HS.SP.ToString()     ->   SIGSEGV
//
// No diagnostic on any of the three WRITES: a silent wrong value across the
// module boundary, which §2.8.2 clause 1 forbids (an artifact's interface must
// carry what a consumer needs to use its public surface) and §22.12 makes a
// plain member access ("Static methods, properties, fields, and constructors
// are declared with `static`").
//
// ROOT CAUSE (both halves, and they disagreed with each other):
//   * `emitExportedProperty{Getters,Setters}` (LlvmBackend_Setup.cpp) skip a
//     static property outright ("Static properties have no `this` parameter and
//     are handled separately by the static-field path") — but no separate path
//     ever existed for a COMPUTED static property, so no module ever DEFINED a
//     cross-module symbol for one.
//   * `declareImportedProperty{Getters,Setters}` carry no static filter, so the
//     importer declared the INSTANCE-shaped `T get(ptr)` / `void set(ptr, T)`
//     stub for it anyway. The call never fired: for a static property the
//     receiver expression is a TYPE NAME, so the receiver resolution produced
//     nothing pointer- or aggregate-shaped and the lowering returned false /
//     nullptr — the write then fell through to the generic `lvalueAddr`
//     fallback, which cannot resolve a PropertySymbol target at all (the
//     #2400/#2862 drop shape), and the read left `lowerExpr` as a bare nullptr.
//
// THE FIX: give the static case its own accessor ABI — `bz$<Class>$<Prop>$
// spropget() -> T` and `bz$<Class>$<Prop>$spropset(T) -> void`, a DIFFERENT
// symbol name for a different signature (a C symbol carries no type, so sharing
// the instance name with a shorter argument list would turn any future
// exporter/importer disagreement into a silently mis-registered argument rather
// than a link error). `ensurePropertyAccessorFn` gained the static-aware branch
// that builds the bodies (no `this` parameter, no §28.2 checked-disposal guard,
// `value` at argument 0, and — for a static AUTO-property — the module-level
// global §23.1/#743 already allocates instead of an instance slot); the
// importer's read, `:=`, `+=` and `++`/`--` paths call them, preceded by the
// same `emitEnsureStaticInit` a cross-module static FIELD access already emits
// so §22.13's lazy initialization runs on first use (T4 pins this).
//
// The exporter's and importer's gates are deliberately the SAME five facts, all
// of which the importer holds in metadata — public non-generic Class/Record
// parent, public property, `static`, non-indexer, accessor face present. An
// importer gate that can be true where the exporter's is false is an
// `undefined reference` at link time: this fix's own first draft excluded a
// static AUTO-property on the exporter side only (the importer cannot detect
// one — `MetaMember::isAutoProperty` is deliberately forced false for every
// static, since it means "instance storage") and the grid link-failed on
// `SAuto.A`. T5 pins the auto shape.
//
// FAIL-WITHOUT / PASS-WITH: demonstrated mechanically by reverting the four
// src/ files to HEAD (scratch-copied patch + `git checkout --`, never `git
// stash`), rebuilding, and confirming T1-T5 fail with exactly the measured
// wrong values above (and T3's crash), then re-applying the byte-identical
// patch and confirming all pass. T6/T7 are controls that pass both ways.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) was race-prone even with
// unique_stem: parallel shards racing on the SAME /tmp parent dir overwrote
// each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L133 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir2885() {
    return wtScratchDir("wt-2885");
}

struct TempFile2885 {
    std::string path;
    explicit TempFile2885(std::string p) : path(std::move(p)) {}
    ~TempFile2885() { std::remove(path.c_str()); }
};

void writeSource2885(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, then compile `appSrc` against it, link, and
// run in a SEPARATE process, capturing stdout. Returns {exitCode, stdout};
// {-2, ""} when the toolchain/dependency could not be produced (SKIP),
// {-1, ""} on compile/link failure of the importing side.
//
// stdout is the oracle rather than the exit code: WEXITSTATUS masks to 8 bits
// (tests/README.md) and several of the values pinned here exceed 255. ASan is
// deliberately NOT used — the DEPENDENCY artifact is compiled by a separate
// driver invocation and is not instrumented, so a cross-module run is not a
// sanitizer oracle; asserted values are.
std::pair<int, std::string> compileImportRunCapture2885(const std::string& libSrc,
                                                        const std::string& appSrc,
                                                        const std::string& tag) {
    auto dir = wtDir2885();
    std::string stem = "bz2885_" + tag + "_" + unique_stem("");
    TempFile2885 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2885 aObj((dir / (stem + "_a.o")).string());
    writeSource2885(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile2885 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2885 bExe((dir / (stem + "_b")).string());
    writeSource2885(bSrc.path, appSrc);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        if (!driver.compileFile(bSrc.path, bExe.path, deps).success)
            return {-1, ""};
    }
    std::string cmd = bExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

bool has2885(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The exporting module used by T1-T4: a computed (block-form) static property
// over a static field whose initializer is a static METHOD CALL, so §22.13's
// lazy initialization is observable (a first read must see 5, not 0).
const char* kLib2885 =
    "module StatLib2885; "
    "public class SHost2885 "
    "  public static _q: int := Seed(); "
    "  public static method Seed(): int "
    "    return 5; "
    "  end Seed; "
    "  public static property SP: int "
    "    get => _q + 100; "
    "    set => do _q := value + 1; end; "
    "  end; "
    "end SHost2885;";

}  // namespace

// ===========================================================================
// T1 — the #2885 repro: `HS.SP := 7` across the module boundary. The setter
// body stores `value + 1`, so the backing static field must read 8. Pre-fix:
// 0, with no diagnostic — the write was discarded outright.
// ===========================================================================
BEZEL_TEST(xmod_static_property_2885, plain_assign_reaches_the_imported_setter) {
    auto [code, out] = compileImportRunCapture2885(
        kLib2885,
        "module StatApp2885T1; import StatLib2885; "
        "public class C "
        "  public static method Main(): int "
        "    SHost2885.SP := 7; "
        "    Console.WriteLine(\"q=\" + SHost2885._q.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t1");
    if (code == -2) return;  // toolchain unavailable
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2885(out, "q=8"));
}

// ===========================================================================
// T2 — the READ-MODIFY-WRITE spellings, §6.6 and §17.6 ("`x++` is equivalent to
// `x := x + 1`"). Starting from _q = 5: `+=` reads 105, adds 1, stores 106+1 =
// 107; `++` then reads 207, stores 208+1 = 209; `--` reads 309, stores 308+1 =
// 309. Pre-fix every one of them left _q untouched at 0 (the getter read 0 too,
// so even the arithmetic was wrong, not just the store).
// ===========================================================================
BEZEL_TEST(xmod_static_property_2885, compound_and_increment_reach_the_accessors) {
    auto [code, out] = compileImportRunCapture2885(
        kLib2885,
        "module StatApp2885T2; import StatLib2885; "
        "public class C "
        "  public static method Main(): int "
        "    SHost2885.SP += 1; "
        "    Console.WriteLine(\"a=\" + SHost2885._q.ToString()); "
        "    SHost2885.SP++; "
        "    Console.WriteLine(\"b=\" + SHost2885._q.ToString()); "
        "    SHost2885.SP--; "
        "    Console.WriteLine(\"c=\" + SHost2885._q.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t2");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2885(out, "a=107"));
    BEZEL_EXPECT_TRUE(has2885(out, "b=209"));
    BEZEL_EXPECT_TRUE(has2885(out, "c=309"));
}

// ===========================================================================
// T3 — the READ side, in the two spellings that failed differently pre-fix:
// bound to a local (`var r := HS.SP;` degraded to 0 through lowerLocalDecl's
// #2434 zero-init backstop) and used inline inside a string concatenation
// (SIGSEGV). Both must now produce the getter's value, 105 — which is also
// T4's §22.13 evidence.
// ===========================================================================
BEZEL_TEST(xmod_static_property_2885, read_binds_and_inlines_correctly) {
    auto [code, out] = compileImportRunCapture2885(
        kLib2885,
        "module StatApp2885T3; import StatLib2885; "
        "public class C "
        "  public static method Main(): int "
        "    var r := SHost2885.SP; "
        "    Console.WriteLine(\"bound=\" + r.ToString()); "
        "    Console.WriteLine(\"inline=\" + SHost2885.SP.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t3");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2885(out, "bound=105"));
    BEZEL_EXPECT_TRUE(has2885(out, "inline=105"));
}

// ===========================================================================
// T4 — §22.13 across the artifact boundary: "a type's static fields are
// initialized lazily on first use". The FIRST touch of SHost2885 from the
// importing module is the property read itself, and `_q`'s initializer is a
// call to `Seed()` (5) — so the getter must observe 105, not 100 (initializer
// never ran) and not 0 (nothing ran). The stub call is preceded by the same
// `bz$<T>$ensureinit` call a cross-module static FIELD access already emits.
// ===========================================================================
BEZEL_TEST(xmod_static_property_2885, lazy_static_init_runs_before_the_getter) {
    auto [code, out] = compileImportRunCapture2885(
        kLib2885,
        "module StatApp2885T4; import StatLib2885; "
        "public class C "
        "  public static method Main(): int "
        "    Console.WriteLine(\"first=\" + SHost2885.SP.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t4");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2885(out, "first=105"));
    BEZEL_EXPECT_FALSE(has2885(out, "first=100"));
}

// ===========================================================================
// T5 — the two shapes the exporter/importer gates must agree on, in one build:
//   * a static AUTO-property (§22.12 rule 2's "static field or auto-property",
//     §23.1/#743 storage) — the shape whose asymmetric first-draft exclusion
//     link-failed;
//   * a REFERENCE-typed (string) static property, reached through a
//     `static owned readonly` holder because §22.12 rule 2 rejects a mutable
//     `string` static outright. Pins `+=` on a reference type (string concat)
//     as well as plain get/set.
// ===========================================================================
BEZEL_TEST(xmod_static_property_2885, auto_and_reference_typed_static_properties) {
    auto [code, out] = compileImportRunCapture2885(
        "module StatLib2885T5; "
        "public class SAuto2885 "
        "  public static property A: int := 3; "
        "end SAuto2885; "
        "public class Holder2885 "
        "  public v: string := \"a\"; "
        "end Holder2885; "
        "public class SStr2885 "
        "  private static owned readonly _h: Holder2885 := new Holder2885(); "
        "  public static property Name: string "
        "    get => _h.v; "
        "    set => do _h.v := value; end; "
        "  end; "
        "end SStr2885;",
        "module StatApp2885T5; import StatLib2885T5; "
        "public class C "
        "  public static method Main(): int "
        "    Console.WriteLine(\"auto0=\" + SAuto2885.A.ToString()); "
        "    SAuto2885.A := 11; "
        "    Console.WriteLine(\"auto1=\" + SAuto2885.A.ToString()); "
        "    SAuto2885.A += 1; "
        "    Console.WriteLine(\"auto2=\" + SAuto2885.A.ToString()); "
        "    SAuto2885.A++; "
        "    Console.WriteLine(\"auto3=\" + SAuto2885.A.ToString()); "
        "    Console.WriteLine(\"str0=\" + SStr2885.Name); "
        "    SStr2885.Name := \"zz\"; "
        "    Console.WriteLine(\"str1=\" + SStr2885.Name); "
        "    SStr2885.Name += \"!\"; "
        "    Console.WriteLine(\"str2=\" + SStr2885.Name); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t5");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2885(out, "auto0=3"));
    BEZEL_EXPECT_TRUE(has2885(out, "auto1=11"));
    BEZEL_EXPECT_TRUE(has2885(out, "auto2=12"));
    BEZEL_EXPECT_TRUE(has2885(out, "auto3=13"));
    BEZEL_EXPECT_TRUE(has2885(out, "str0=a"));
    BEZEL_EXPECT_TRUE(has2885(out, "str1=zz"));
    BEZEL_EXPECT_TRUE(has2885(out, "str2=zz!"));
}

// ===========================================================================
// T6 — ACCEPT CONTROL (passes before and after): the non-generic, non-static
// imported computed property #2862 fixed. This fix must not disturb it — read,
// `:=`, `+=` and `++` all still go through the INSTANCE `$propget`/`$propset`
// stubs. From _n = 1: read 101; `:= 7` stores 8; `+= 1` reads 108 -> 110;
// `++` reads 210 -> 212.
// ===========================================================================
BEZEL_TEST(xmod_static_property_2885, instance_property_control_unchanged) {
    auto [code, out] = compileImportRunCapture2885(
        "module StatLib2885T6; "
        "public class HPlain2885 "
        "  public _n: int := 1; "
        "  public property Name: int "
        "    get => _n + 100; "
        "    set => do _n := value + 1; end; "
        "  end; "
        "end HPlain2885;",
        "module StatApp2885T6; import StatLib2885T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new HPlain2885(); "
        "    Console.WriteLine(\"get=\" + h.Name.ToString()); "
        "    h.Name := 7; "
        "    Console.WriteLine(\"set=\" + h._n.ToString()); "
        "    h.Name += 1; "
        "    Console.WriteLine(\"cmp=\" + h._n.ToString()); "
        "    h.Name++; "
        "    Console.WriteLine(\"inc=\" + h._n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t6");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2885(out, "get=101"));
    BEZEL_EXPECT_TRUE(has2885(out, "set=8"));
    BEZEL_EXPECT_TRUE(has2885(out, "cmp=110"));
    BEZEL_EXPECT_TRUE(has2885(out, "inc=212"));
}

// ===========================================================================
// T7 — ABI-SURFACE CONTROL: a NON-PUBLIC static property must NOT become part
// of the exported surface (both gates require `public`), and the module must
// still link and behave — the declaring module reaches its own private static
// property through a public static method. This is the shape that would break
// if the exporter's gate were widened past what the importer can see.
// ===========================================================================
BEZEL_TEST(xmod_static_property_2885, private_static_property_not_exported) {
    auto [code, out] = compileImportRunCapture2885(
        "module StatLib2885T7; "
        "public class SPriv2885 "
        "  public static _p: int := 0; "
        "  private static property Hidden: int "
        "    get => _p + 1; "
        "    set => do _p := value; end; "
        "  end; "
        "  public static method Poke(): int "
        "    Hidden := 9; "
        "    return Hidden; "
        "  end Poke; "
        "end SPriv2885;",
        "module StatApp2885T7; import StatLib2885T7; "
        "public class C "
        "  public static method Main(): int "
        "    Console.WriteLine(\"poke=\" + SPriv2885.Poke().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t7");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2885(out, "poke=10"));
}

// ===========================================================================
// T8 — the RECORD arm of the gate (§9.4 admits body-form record properties;
// the exporter's gate is Class-or-Record, as #2489 established for the
// instance emitters). Same shape as T1/T2 on a record host.
// ===========================================================================
BEZEL_TEST(xmod_static_property_2885, record_host_static_property) {
    auto [code, out] = compileImportRunCapture2885(
        "module StatLib2885T8; "
        "public record RStat2885 "
        "  public x: int; "
        "  public static _r: int := 0; "
        "  public static property RP: int "
        "    get => _r + 100; "
        "    set => do _r := value + 1; end; "
        "  end; "
        "end RStat2885;",
        "module StatApp2885T8; import StatLib2885T8; "
        "public class C "
        "  public static method Main(): int "
        "    Console.WriteLine(\"get=\" + RStat2885.RP.ToString()); "
        "    RStat2885.RP := 7; "
        "    Console.WriteLine(\"set=\" + RStat2885._r.ToString()); "
        "    RStat2885.RP += 1; "
        "    Console.WriteLine(\"cmp=\" + RStat2885._r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t8");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2885(out, "get=100"));
    BEZEL_EXPECT_TRUE(has2885(out, "set=8"));
    BEZEL_EXPECT_TRUE(has2885(out, "cmp=110"));
}

}  // namespace bezel::module
