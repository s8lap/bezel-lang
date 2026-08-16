// fix #3196 (§22.5 "Optional parameters and named arguments" / §22.10 "primary
// constructors" / §22.8 step 0 / §2.8.3 rule 4; spec v0.254) — a NAMED argument
// to a §22.10 PRIMARY CONSTRUCTOR lowers correctly on BOTH sides of a module
// boundary.
//
// ── WHAT WAS BROKEN ────────────────────────────────────────────────────────
//
// Before #3196 the codegen path `storePrimaryCtorPositionalArgs`
// (`src/codegen/LlvmBackend_Exprs2.cpp`, the §22.8 step 0 helper #3141/#3162
// landed) paired `ne.args[i]` with the field at position i positionally. §22.5
// admits named arguments (`new PBox(W := 4, V := 3)` binds `W` to field 1 and
// `V` to field 0), and `checkPositionalNewArgsFromSymbols` already computed
// the §22.5 binding as a local `fieldToArg` vector — but the lowering read
// `ne.args[0]` and `ne.args[1]` in the call's source order and stored them into
// fields 0 and 1 in order, silently SWAPPING V and W. The result was a
// calloc-zero on the IMPORTED side (the loud `anyNamed` decline `d7eef83`
// added to `checkNewExprArgs` at sema line ~10898 in fix #3162 was the
// diagnosed defense) and a silent-zero OBJECT on the LOCAL side (the
// `if (!ctx_.isImported(t)) return;` deferral at sema line ~10824 — #3163's
// domain — let the local `new LocalPBox(W := 4, V := 3)` slip past both sema
// and codegen with no diagnostic and a zeroed object, measured for the
// `class PBox(V: int, W: int)` repro as `Sum() = 0`).
//
// ── THE FIX SHAPE ──────────────────────────────────────────────────────────
//
// `SemaContext::setNewExprPositionalBinding` publishes the same `fieldToArg`
// vector `checkPositionalNewArgsFromSymbols` computed. Codegen reads it
// (`newExprPositionalBinding(ne)`) and uses it to drive the per-field
// `ne.args[argIdx]` lookup; when not published (combined primary+explicit-ctor
// form, #3163's other half), the helper falls back to position-i pairing, the
// pre-#3196 behavior. The §22.5 binding has exactly one body in the compiler,
// so sema and codegen cannot drift from what each accepted.
//
// The loud IMPORTED `anyNamed` decline is removed (the codegen half now lowers
// the named form). The LOCAL semantic deferral (`!isImported(t) return;`) is
// gone too — the local primary-only case now runs the same check the imported
// arm uses, so local named calls produce the same value as their imported
// twins.
//
// ── THE BINDING ROWS (T1–T5) ────────────────────────────────────────────────
//
// T1 — §22.5 named: `new PBox(W := 4, V := 3)` on an IMPORTED
//      `class PBox(V: int, W: int)`. Pre-#3196 this was REJECTED (the loud
//      decline). Pre-#3141 it compiled CLEAN and printed `Sum()=0` (the bug).
//      #3196 lowers it and answers `Sum()=7`.
// T1L — the LOCAL twin: the byte-identical declaration+call in ONE module.
//       Pre-#3196 the local twin compiled CLEAN and printed `Sum()=0`. The
//       defect was symmetric; the fix is symmetric.
// T2 — the REVERSE order (`V := 3, W := 4` listed in declaration order
//      instead of source order): the pre-#3196 codegen pairing's
//      *signature* mistake was to pin the call's positional order — `new
//      PBox(W := 4, V := 3)` swaps to `Sum()=7+0=7`, but `new PBox(V := 3, W
//      := 4)` (the symmetric spelling, in declaration order) would also have
//      swapped to `Sum()=7+0=7` because the named binding happens to match
//      source order by accident. The pre-#3196 lowered `ne.args[0]` regardless
//      of the name, so either spelling silently zeroed. Pin both spellings.
// T3 — a NAMED IMMUTABLE `:= 9`-style default intermixed with a NAMED supply
//      (`PBox(W := 4)` binds only W, leaves V to default; reads `W + V`).
//      Pre-#3196 the under-supplied-named call would have either been
//      rejected (imported, by the loud decline that came BEFORE default
//      argument's lower) or zeroed (local, by the combined
//      allPositional-bail + named-supply shape).
// T4 — the LOCAL RECLAIMED row: #3163's deferral is gone, so the local
//      primary-only construction now refuses the wrong-typed NAME the same way
//      it refused the wrong-typed POSITIONAL: a loud §5.1 reject, not a
//      silent-zero. The pre-#3196 path took a wrong-typed positional silently
//      — pinning that the loud reject is the same path the named form is now
//      on closes the door to the silent-zero twin of the same defect.
//
// ── ORACLES ────────────────────────────────────────────────────────────────
//
// Every row here runs under BOTH gate oracles: `compileFileForTest` drives the
// AOT path with fork-isolated compiles + a separate native exec, so a wrong-
// typed or out-of-bounds read lands as either a signaled exit (the pre-#3196
// defect class) or an ASan report (under `--full`), never as a "did not
// crash" green for the wrong reason.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// A minimal auxiliary to write/read a temp file — same shape every cross-module
// row in this directory uses; nothing test-specific lives here.
struct TempFile3196 {
    std::string path;
    explicit TempFile3196(std::string p) : path(std::move(p)) {}
    ~TempFile3196() { std::remove(path.c_str()); }
};

void writeSource3196(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

constexpr int kDepUnavailable3196 = -2;
constexpr int kRejected3196 = -3;

class StderrCapture3196 {
public:
    explicit StderrCapture3196(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3196() { restore(); }
    std::string text() {
        restore();
        std::ifstream in(path_);
        std::string s((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
        std::remove(path_.c_str());
        return s;
    }

private:
    void restore() {
        if (saved_ < 0) return;
        std::fflush(stderr);
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        saved_ = -1;
    }
    std::string path_;
    int saved_ = -1;
};

bool mentions3196(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The generic two-module runner: compile `libSrc` to an object via
// `compileObject`, then compile `appSrc` against it as a dependency and run
// the produced binary. Mirrors `compileImportAndRun3162` so a `KEEP_*` of any
// shape can be cross-checked with the #3162 test if/when the affordances meet.
int compileImportAndRun3196(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3196_");
    TempFile3196 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3196 aObj((dir / (stem + "_a.o")).string());
    writeSource3196(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3196;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3196;

    TempFile3196 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3196 bExe((dir / stem).string());
    writeSource3196(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3196 cap((dir / (stem + ".err")).string());
        ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                 .success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3196;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module twin — the same program in one compilation. The fields
// the codegen reads (positional field index, types, etc.) are bit-identical
// between this and the import-side counterpart, so a divergence between
// `compileImportAndRun3196` and `compileAndRun3196` (same source, different
// modules) on the same source body is a #3196 cross-module bug specifically.
int compileAndRun3196(const std::string& src, std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3196L_");
    TempFile3196 s((dir / (stem + ".bzl")).string());
    TempFile3196 exe((dir / stem).string());
    writeSource3196(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3196 cap((dir / (stem + ".err")).string());
        ok = bezel::test::compileFileForTest(driver, s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3196;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The shared `class PBox` declaration. `V` then `W` in source order; the
// named-arg rows exercise `W := 4, V := 3` (reverse) and `V := 3, W := 4`
// (forward) and one of `PBox(W := 4)` (default-fill on V).
const char* kDecls3196 =
    // The ordinary primary-ctor class with two ints.
    "public class PBox(V: int, W: int) "
    "  public method Sum(): int "
    "    return V + W; "
    "  end Sum; "
    "end PBox; "
    // A primary-ctor class with ALL parameters defaulted, to exercise the
    // named-default cross (T3): omitting V (which has `:= 9`) should fill
    // it from the default and the call `new DefPBox(W := 4)` should answer
    // 4 + 9 = 13. With only a tail default (`V: int, W: int := 9`) the
    // named-omit-V call would still be a §22.5 reject (V is required), so
    // both slots carry defaults to keep this row's claim simple.
    "public class DefPBox(V: int := 9, W: int := 10) "
    "  public method Sum(): int "
    "    return V + W; "
    "  end Sum; "
    "end DefPBox;";

std::string lib3196(const std::string& tag) {
    return "module Lib3196" + tag + "; " + kDecls3196;
}
std::string app3196(const std::string& tag, const std::string& body) {
    return "module App3196" + tag + "; import Lib3196" + tag + "; "
           "public class C" + tag + " " + body + " end C" + tag + ";";
}
std::string twin3196(const std::string& tag, const std::string& body) {
    return "module Twin3196" + tag + "; " + kDecls3196 +
           " public class C" + tag + " " + body + " end C" + tag + ";";
}

}  // namespace

// ===========================================================================
// T1 — §22.5 / §22.10 / §22.8 step 0: a NAMED argument to an IMPORTED primary
// constructor LOWERS CORRECTLY.
//
// `new PBox(W := 4, V := 3)` binds `W → 1, V → 0` (§22.5). `Sum()` = V + W =
// 3 + 4 = 7. Pre-#3196 this shape was REJECTED outright ("`new PBox` binds a
// NAMED argument to an IMPORTED class's primary constructor, which is not yet
// supported (§22.5 / §22.10 / §2.8.3 rule 4, #3162)") — measured on the parent
// commit `d7eef83` (#3162's merge). The loud decline was the diagnostic
// counterpart of the silent-zero object the local twin printed; the
// `checkPositionalNewArgsFromSymbols` sema check already bound the call
// correctly, so the gap was purely a lowering one. #3196 lifts the decline on
// the argument that the lowering has caught up.
// ===========================================================================
BEZEL_TEST(xmod_named_arg_imported_primary_ctor_3196,
           t1_named_arg_imported_constructor_lowers_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(W := 4, V := 3); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileImportAndRun3196(lib3196("T1"), app3196("T1", body));
    if (r == kDepUnavailable3196) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T1L — the LOCAL twin: the same program in one module. Pre-#3196 the local
// twin compiled CLEAN and printed `Sum()=0` — measured on `d7eef83`/#3141's
// merge — because the local class deferred at sema line ~10824, the codegen
// named-supply bail at line ~2683 dropped the store loop, and the resulting
// object never saw either argument. The fix lifts the sema deferral for the
// simple primary-only case so the local twin now runs the same check the
// imported path uses, and the codegen reads the published binding instead of
// the silent-zero fallback. Symmetry is the whole point of T1L — divergence
// between T1 and T1L is a #3196 cross-module bug specifically.
// ===========================================================================
BEZEL_TEST(xmod_named_arg_imported_primary_ctor_3196,
           t1_l_named_arg_local_constructor_lowers_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(W := 4, V := 3); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileAndRun3196(twin3196("T1L", body));
    if (r == kDepUnavailable3196) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T2 — §22.5: REORDERED named arguments. `new PBox(V := 3, W := 4)` lists the
// names in declaration order; the binding `V → 0, W → 1` happens to coincide
// with positional order, so a buggy fix that still pairs `ne.args[i]` with
// field `i` would *also* answer 7 — a swap-fix cannot be distinguished from
// the correct fix on T2 alone.
//
// The pre-#3196 codegen happened to land on `4` in V's slot (because call
// source order `W := 4, V := 3` paired `ne.args[0]`=4 with field 0=V, and
// `ne.args[1]`=3 with field 1=W). The shape `new PBox(V := 3, W := 4)` — in
// declaration order — would have read `Sum()=3+4=7` by coincidence on the same
// defective pairing, just because the §22.5 binding happened to be the same as
// the positional order.
//
// This row pins that BOTH spellings agree on 7 in the corrected compiler, and
// is paired with T1 to make the fix's indifference to source order explicit:
// a fix that swapped fields 0/1 based on positional order would PASS T2 alone
// while FAILING T1 — together T1+T2 bracket the §22.5 ordering.
// ===========================================================================
BEZEL_TEST(xmod_named_arg_imported_primary_ctor_3196,
           t2_reversed_named_arg_imported_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(V := 3, W := 4); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileImportAndRun3196(lib3196("T2"), app3196("T2", body));
    if (r == kDepUnavailable3196) return;
    BEZEL_EXPECT_EQ(r, 7);
}

BEZEL_TEST(xmod_named_arg_imported_primary_ctor_3196,
           t2_l_reversed_named_arg_local_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(V := 3, W := 4); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileAndRun3196(twin3196("T2L", body));
    if (r == kDepUnavailable3196) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T3 — §22.5 + §22.10: NAMED argument + DEFAULTED slot. `new DefPBox(W := 4)`
// supplies W by name and OMITS V — V's `:= 9` carries a default, so the
// binding reads `V → -1, W → 0`. The runtime default arm
// (`storePrimaryCtorPositionalArgs` #3202's per-slot default fill on `argIdx
// == -1`, which the §22.5 binding published by `checkPositionalNewArgsFromSymbols`
// surfaces) materializes V from `:= 9`, and `Sum()` reads 9 + 4 = 13.
//
// Both V AND W carry defaults: pre-#3202/#3162 this also exercises the
// under-supplied-named slot's default carriage — the same `fieldToArg[j] ==
// -1 → default-fill` arm #3202 added for the positional case fires under a
// NAMED-supplied other slot, with no diagnostic text changes — the codegen
// half is one binding, with one body.
//
// Pre-#3196 the imported side was REJECTED outright (`anyNamed` decline); the
// local twin compiled CLEAN and read `Sum()` = 0 + 0 = 0 (the silent-zero
// family as T1's local twin, since the named-supplied-W-only call still
// bypassed any per-slot source). After #3196 both sides pin the same 13.
// ===========================================================================
BEZEL_TEST(xmod_named_arg_imported_primary_ctor_3196,
           t3_named_arg_with_defaulted_slot_imported_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new DefPBox(W := 4); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileImportAndRun3196(lib3196("T3"), app3196("T3", body));
    if (r == kDepUnavailable3196) return;
    BEZEL_EXPECT_EQ(r, 13);
}

BEZEL_TEST(xmod_named_arg_imported_primary_ctor_3196,
           t3_l_named_arg_with_defaulted_slot_local_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new DefPBox(W := 4); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileAndRun3196(twin3196("T3L", body));
    if (r == kDepUnavailable3196) return;
    BEZEL_EXPECT_EQ(r, 13);
}

// ===========================================================================
// T4 — §5.1: a wrong-typed NAMED argument is REJECTED, on BOTH sides.
//
// Pre-#3196 the local twin compiled CLEAN (`Sum()=0+0=0`); the imported side
// was rejected for the wrong reason — the `anyNamed` decline fired before the
// §5.1 check had a chance to fire on the value type. #3196 lifts the
// `anyNamed` decline and activates the same §5.1 path the wrong-typed
// positional already runs (see T9 of `xmod_imported_primary_ctor_3162_test.cpp`
// for the pre-existing positional twin). Pinning the named-share-same-message
// keeps a future re-broadening of the silent-zero fall through loudly.
// ===========================================================================
BEZEL_TEST(xmod_named_arg_imported_primary_ctor_3196,
           t4_named_arg_wrong_typed_imported_rejected_5_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(V := \"hello\", W := 4); "
        "    return b.Sum(); "
        "  end Main;";
    std::string diag;
    int r = compileImportAndRun3196(lib3196("T4"), app3196("T4", body), &diag);
    if (r == kDepUnavailable3196) return;
    BEZEL_EXPECT_EQ(r, kRejected3196);
    // The named form §5.1-pinholes through the SAME §5.1 reject the
    // positional twin already pinholes through — the diagnostic names the
    // conversion failure rather than the named shape.
    BEZEL_EXPECT_TRUE(mentions3196(diag, "not implicitly convertible"));
    BEZEL_EXPECT_TRUE(mentions3196(diag, "\xC2\xA7""5.1"));
}

BEZEL_TEST(xmod_named_arg_imported_primary_ctor_3196,
           t4_l_named_arg_wrong_typed_local_rejected_5_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(V := \"hello\", W := 4); "
        "    return b.Sum(); "
        "  end Main;";
    std::string diag;
    int r = compileAndRun3196(twin3196("T4L", body), &diag);
    if (r == kDepUnavailable3196) return;
    BEZEL_EXPECT_EQ(r, kRejected3196);
    BEZEL_EXPECT_TRUE(mentions3196(diag, "not implicitly convertible"));
    BEZEL_EXPECT_TRUE(mentions3196(diag, "\xC2\xA7""5.1"));
}

}  // namespace bezel::module
