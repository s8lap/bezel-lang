// fix #3203 (§11.2 / §2.8.3 rule 4, v0.254): the signature comparison the
// §11.2 conformance check runs on a witness's parameter/return types MUST
// distinguish inline-array sizes when the witness is IMPORTED, exactly as it
// does for a LOCAL witness. §2.8.3 rule 4 forbids a cross-module obligation
// from being silently less strict than its single-module twin.
//
// ── THE DEFECT, AS IT WAS MEASURED IN PER-FIX REVIEW OF #3194 ──────────────
//
// `Resolver::checkAbstractImplementations` (§11.2) calls a `typesMatch` lambda
// that, for an InlineArray pair, falls through to `Resolver::typeRefEqual`
// (TypeResolution_Checks.cpp:1385-1400). `typeRefEqual` distinguishes two
// inline arrays by `arraySizeValue` when both are FOLDED (arraySizeValue >= 0,
// the #2001 rule). For the LOCAL case, the impl's TypeRef reaches the
// comparison with its size already folded by TypeResolution (#734), so the
// comparison correctly rejects an arity mismatch.
//
// For the IMPORTED case the impl may be inherited from a DEPENDENCY (the
// #3194 shape); the witness's parameter types ride `resolvedDeclType` (no
// ast::Type node), and were reconstructed by ModuleLoader. Prior to #3183,
// ModuleLoader did NOT reconstruct `arraySizeValue` (it "deliberately does not
// reconstruct `arraySize`" — old comment at src/module/ModuleLoader.cpp:224-231),
// so the imported witness's parameter type reached `typeRefEqual` with
// `arraySizeValue == -1` and every arity compared equal. The §11.2 check
// ACCEPTED the mismatch; the itable slot stored a witness that expected a wider
// inline array than the interface obligation passed.
//
// The dangerous direction was observable: impl `HArr(a: [8]int): int` returning
// `a[7]`, interface `[2]int`, caller passing `[11, 22]`:
//   local twin → REJECTED (§11.2 mismatch on parameter 1)
//   imported   → ACCEPTED, then read past the caller's 2-element array
//                → returned 11 (an out-of-range read through the itable,
//                memory-unsafe per §2.8.3 rule 4)
//
// ── HOW IT IS FIXED ON `main` ──────────────────────────────────────────────
//
// #3183 (commit ad78436, "carry an inline array's SIZE across a module
// boundary") populated `MetaType::arraySize` on the wire and the loader
// restores it into `TypeRef::arraySizeValue`. With that, `typeRefEqual`'s
// existing #2001 rule distinguishes the two sizes for an imported witness
// exactly as it always did for a local one — no new comparison site is needed,
// and the issue's "may well be one fix" guess (that #3183 also fixes this site)
// was correct.
//
// This file pins that contract by exercising the four shapes the issue names
// and the one shape §2.8.3 rule 4 turns into a regression: identical behavior
// for the local twin and the imported build. Each T# runs BOTH arrangements
// against the same predicate so a future re-break of either side fails the
// row (silent accept on either side is just as bad as a disagreement).
//
// ── AXES COVERED ───────────────────────────────────────────────────────────
//
//   parameter arity  (T1 reject, T2 accept)         — the issue's core defect
//   return arity     (T3 reject, T4 accept)         — symmetric axis the issue names
//   T → T? widening  (T5 reject: still size-strict under widening)
//   element equality (T6 accept: same element, same size — no false reject)
//   ownedness        (T7 reject: owned ≠ unowned even at same size,
//                     over a class element — §15.1.1 admits no other kind)
//
// All seven rows MUST pass on `main`. T1 / T3 / T5 / T7 are the rows a
// pre-#3183 build silently accepts and #3183 makes discriminate; T2 / T4 / T6
// are the over-rejection controls that #3183 (a pure wire-carrier) must not
// disturb. (The "nested-array" axis from the lane-protocol grid is not
// representable in source: multi-dimensional inline arrays are deferred, so
// it cannot be expressed as a test.)

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3203 {
    std::string path;
    explicit TempFile3203(std::string p) : path(std::move(p)) {}
    ~TempFile3203() { std::remove(path.c_str()); }
};

void writeSource3203(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled3203 {
    bool depOk = true;
    bool success = false;
    std::string err;
};

template <typename Fn>
std::string captureStderr3203(Fn&& fn) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz3203_err_" + unique_stem("") + ".txt"))
            .string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

Compiled3203 compileImport3203(const std::string& libSrc,
                                const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3203_");
    TempFile3203 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3203 aObj((dir / (stem + "_a.o")).string());
    writeSource3203(aSrc.path, libSrc);
    Compiled3203 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile3203 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3203 bExe((dir / (stem + "_b")).string());
    writeSource3203(bSrc.path, appSrc);
    r.err = captureStderr3203([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.success =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    });
    return r;
}

Compiled3203 compileLocal3203(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3203l_");
    TempFile3203 aSrc((dir / (stem + ".bzl")).string());
    TempFile3203 aExe((dir / stem).string());
    writeSource3203(aSrc.path, src);
    Compiled3203 r;
    r.err = captureStderr3203([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

bool mentions3203(const Compiled3203& c, const std::string& needle) {
    return c.err.find(needle) != std::string::npos;
}

// The interface the issue's measurements used. The dual shape — the parameter
// type AND the return type carry an inline array — is deliberate, so the
// per-axis rows below can pick one or the other and exercise both sites in the
// §11.2 check (parameter-type at TypeResolution_Checks.cpp:4382 and
// return-type at 4433, both going through `typesMatch` → `typeRefEqual`).
const char* kIface3203 =
    "public interface IBig3203 "
    "  method HP(a: [2]int): int; "
    "  method HR(): [2]int; "
    "end IBig3203; ";

// The dependency's body for the IMPORTED-build rows: just the interface
// (no implementations) so the witness sits in the importer.
const std::string kLib3203 =
    std::string("module VL3203; ") + kIface3203;

// ===========================================================================
// T1 — §11.2 / §2.8.3 rule 4: PARAMETER arity mismatch (the issue's named
// defect). Interface `HP(a: [2]int): int`, witness `HP(a: [8]int): int`. The
// dangerous direction (impl wider than interface) is the one whose acceptance
// turns into a stack-buffer-overflow at runtime; the narrower direction is
// also wrong but doesn't read past the buffer.
//
// Local twin: must reject (§11.2 mismatch on parameter 1).
// Imported:   must reject (otherwise itable dispatch reads past the caller's
//             2-element array).
// ===========================================================================
BEZEL_TEST(xmod_iface_inline_array_arity_imported_3203,
           t1_parameter_arity_mismatch_rejects_both_11_2) {
    const std::string body =
        "public class Big: IBig3203 "
        "  method HP(a: [8]int): int "
        "    return a[7]; "
        "  end HP; "
        "  method HR(): [2]int "
        "    return [11, 22]; "
        "  end HR; "
        "end Big;";
    Compiled3203 twin = compileLocal3203(
        std::string("module VS3203T1; ") + kIface3203 + body);
    BEZEL_EXPECT_TRUE(!twin.success);
    BEZEL_EXPECT_TRUE(mentions3203(twin, "11.2"));
    BEZEL_EXPECT_TRUE(mentions3203(twin, "parameter 1"));

    Compiled3203 imp = compileImport3203(
        kLib3203,
        std::string("module VA3203T1; import VL3203; ") + body);
    if (!imp.depOk) return;
    BEZEL_EXPECT_TRUE(!imp.success);
    BEZEL_EXPECT_TRUE(mentions3203(imp, "11.2"));
    BEZEL_EXPECT_TRUE(mentions3203(imp, "parameter 1"));
}

// ===========================================================================
// T2 — the matching case. Interface and witness agree on `[2]int`. Both
// arrangements must compile and run; this is the over-rejection control
// (#3183 is a pure wire-carrier and must not disturb this axis).
// ===========================================================================
BEZEL_TEST(xmod_iface_inline_array_arity_imported_3203,
           t2_parameter_arity_match_accepts_both_11_2) {
    const std::string body =
        "public class Big: IBig3203 "
        "  method HP(a: [2]int): int "
        "    return a[1]; "
        "  end HP; "
        "  method HR(): [2]int "
        "    return [11, 22]; "
        "  end HR; "
        "end Big; "
        "public class App3203 "
        "  public static method Main(): int "
        "    var b := new Big(); "
        "    var arr: [2]int := [7, 9]; "
        "    if b.HP(arr) <> 9 then return 1; end if; "
        "    if b.HR()[0] <> 11 then return 2; end if; "
        "    return 0; "
        "  end Main; "
        "end App3203;";
    {
        Compiled3203 twin = compileLocal3203(
            std::string("module VS3203T2; ") + kIface3203 + body);
        if (!twin.depOk) return;
        BEZEL_EXPECT_TRUE(twin.success);
    }
    Compiled3203 imp = compileImport3203(
        kLib3203,
        std::string("module VA3203T2; import VL3203; ") + body);
    if (!imp.depOk) return;
    BEZEL_EXPECT_TRUE(imp.success);
}

// ===========================================================================
// T3 — RETURN arity mismatch. Interface `HR(): [2]int`, witness `HR(): [8]int`.
// The comparison site is `typesMatch(irtSub, crtSub)` at
// TypeResolution_Checks.cpp:4433; the same `typeRefEqual` fallthrough runs
// here too, so a pre-#3183 build accepts this silently as well. Both sides
// must reject.
// ===========================================================================
BEZEL_TEST(xmod_iface_inline_array_arity_imported_3203,
           t3_return_arity_mismatch_rejects_both_11_2) {
    const std::string body =
        "public class Big: IBig3203 "
        "  method HP(a: [2]int): int "
        "    return a[0]; "
        "  end HP; "
        "  method HR(): [8]int "
        "    return [1, 2, 3, 4, 5, 6, 7, 8]; "
        "  end HR; "
        "end Big;";
    Compiled3203 twin = compileLocal3203(
        std::string("module VS3203T3; ") + kIface3203 + body);
    BEZEL_EXPECT_TRUE(!twin.success);
    BEZEL_EXPECT_TRUE(mentions3203(twin, "11.2"));
    BEZEL_EXPECT_TRUE(mentions3203(twin, "return type"));

    Compiled3203 imp = compileImport3203(
        kLib3203,
        std::string("module VA3203T3; import VL3203; ") + body);
    if (!imp.depOk) return;
    BEZEL_EXPECT_TRUE(!imp.success);
    BEZEL_EXPECT_TRUE(mentions3203(imp, "11.2"));
    BEZEL_EXPECT_TRUE(mentions3203(imp, "return type"));
}

// ===========================================================================
// T4 — return arity matching. Both arrange must accept and run.
// ===========================================================================
BEZEL_TEST(xmod_iface_inline_array_arity_imported_3203,
           t4_return_arity_match_accepts_both_11_2) {
    const std::string body =
        "public class Big: IBig3203 "
        "  method HP(a: [2]int): int "
        "    return a[0]; "
        "  end HP; "
        "  method HR(): [2]int "
        "    return [31, 32]; "
        "  end HR; "
        "end Big; "
        "public class App3203 "
        "  public static method Main(): int "
        "    var b := new Big(); "
        "    var arr: [2]int := [0, 0]; "
        "    if b.HP(arr) <> 0 then return 1; end if; "
        "    if b.HR()[1] <> 32 then return 2; end if; "
        "    return 0; "
        "  end Main; "
        "end App3203;";
    {
        Compiled3203 twin = compileLocal3203(
            std::string("module VS3203T4; ") + kIface3203 + body);
        if (!twin.depOk) return;
        BEZEL_EXPECT_TRUE(twin.success);
    }
    Compiled3203 imp = compileImport3203(
        kLib3203,
        std::string("module VA3203T4; import VL3203; ") + body);
    if (!imp.depOk) return;
    BEZEL_EXPECT_TRUE(imp.success);
}

// ===========================================================================
// T5 — §11.2 widening on the parameter side, but with a size mismatch the
// widening does NOT cover. Interface `HP(a: [2]int): int`, witness
// `HP(a: [8]int?): int`. The §11.2 widening `T → T?` admits a nullable impl
// parameter where the interface expects a non-null one; but a SIZE mismatch
// is orthogonal to the widening and is still rejected. Both sides must reject.
// ===========================================================================
BEZEL_TEST(xmod_iface_inline_array_arity_imported_3203,
           t5_widening_does_not_save_arity_mismatch_11_2) {
    const std::string body =
        "public class Big: IBig3203 "
        "  method HP(a: [8]int?): int "
        "    return 0; "
        "  end HP; "
        "  method HR(): [2]int "
        "    return [1, 2]; "
        "  end HR; "
        "end Big;";
    Compiled3203 twin = compileLocal3203(
        std::string("module VS3203T5; ") + kIface3203 + body);
    BEZEL_EXPECT_TRUE(!twin.success);
    BEZEL_EXPECT_TRUE(mentions3203(twin, "11.2"));

    Compiled3203 imp = compileImport3203(
        kLib3203,
        std::string("module VA3203T5; import VL3203; ") + body);
    if (!imp.depOk) return;
    BEZEL_EXPECT_TRUE(!imp.success);
    BEZEL_EXPECT_TRUE(mentions3203(imp, "11.2"));
}

// ===========================================================================
// T6 — element equality at the same arity. Interface and witness both
// `[2]int`; both must compile. Pins that the element-side recursion in
// `typeRefEqual` (line 1400) is not disturbed by the size comparison, and that
// a wider element type would still reject (this row is the accept control).
// ===========================================================================
BEZEL_TEST(xmod_iface_inline_array_arity_imported_3203,
           t6_same_element_same_arity_accepts_both_11_2) {
    const std::string body =
        "public class Big: IBig3203 "
        "  method HP(a: [2]int): int "
        "    return a[0] + a[1]; "
        "  end HP; "
        "  method HR(): [2]int "
        "    return [10, 20]; "
        "  end HR; "
        "end Big; "
        "public class App3203 "
        "  public static method Main(): int "
        "    var b := new Big(); "
        "    var arr: [2]int := [3, 4]; "
        "    if b.HP(arr) <> 7 then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end App3203;";
    {
        Compiled3203 twin = compileLocal3203(
            std::string("module VS3203T6; ") + kIface3203 + body);
        if (!twin.depOk) return;
        BEZEL_EXPECT_TRUE(twin.success);
    }
    Compiled3203 imp = compileImport3203(
        kLib3203,
        std::string("module VA3203T6; import VL3203; ") + body);
    if (!imp.depOk) return;
    BEZEL_EXPECT_TRUE(imp.success);
}

// ===========================================================================
// T7 — REPLACED: nested inline arrays are NOT ADMITTED by the language
// ("an inline array may not be the element of another inline array (`[N][M]T`)
// — multi-dimensional inline arrays are not admitted (§15.1)"), so a
// nested-axis regression cannot even be expressed in source. The T7 slot is
// kept as an ownedness-discrimination axis: `[2 owned]E3203T7` and
// `[2]E3203T7` are distinct types per §15.3 even at the same arity, and
// `typeRefEqual`'s `a->arrayOwned != b->arrayOwned` guard (line 1396) is what
// distinguishes them. Both sides must reject when the impl owns and the
// interface doesn't (or vice versa) — exactly the same predicate the issue's
// "every arity compares equal" claim asserted was missing.
//
// §15.1.1 (v0.307): this row was originally spelled `[2 owned]int` vs
// `[2]int`. The `owned` marker admits only a class or interface element
// ("value types and runtime-managed reference types are not admitted"), so
// `[2 owned]int` is not a well-formed type and the row would have been
// rejected for the WRONG reason — a §15.1.1 element error instead of the
// §11.2 conformance mismatch it exists to pin, which is precisely the
// vacuous-pass shape (right verdict, wrong mechanism). The element type is
// now the class `E3203T7`, exported from the interface's module so the
// imported arrangement resolves it too; the ownedness axis is unchanged, and
// the `mentions3203(..., "11.2")` assertions keep the mechanism honest.
// ===========================================================================
BEZEL_TEST(xmod_iface_inline_array_arity_imported_3203,
           t7_ownedness_distinct_from_unowned_at_same_arity_15_3) {
    const char* iface =
        "public class E3203T7 "
        "  public constructor() end constructor; "
        "end E3203T7; "
        "public interface IBig3203T7 "
        "  method HP(a: [2]E3203T7): int; "
        "end IBig3203T7; ";
    const std::string body =
        "public class Big: IBig3203T7 "
        "  method HP(a: [2 owned]E3203T7): int "
        "    return 0; "
        "  end HP; "
        "end Big;";
    Compiled3203 twin = compileLocal3203(
        std::string("module VS3203T7; ") + iface + body);
    BEZEL_EXPECT_TRUE(!twin.success);
    BEZEL_EXPECT_TRUE(mentions3203(twin, "11.2"));

    Compiled3203 imp = compileImport3203(
        std::string("module VL3203T7; ") + iface,
        std::string("module VA3203T7; import VL3203T7; ") + body);
    if (!imp.depOk) return;
    BEZEL_EXPECT_TRUE(!imp.success);
    BEZEL_EXPECT_TRUE(mentions3203(imp, "11.2"));
}

}  // namespace

}  // namespace bezel::module
