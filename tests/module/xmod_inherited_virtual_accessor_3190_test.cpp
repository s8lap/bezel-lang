// Spec-derived regression test for issue #3190 — family M6 ("imported-ABI
// function maps not consulted") of the MemberSymbol seam, umbrella #3152.
//
// §12.1 / §12.5 (inheritance and dispatch) / §23 (properties) / §23.4
// (indexers) / §2.8.2 clause 1 / §2.8.3 rule 4, v0.254.
// Audit: docs/proposals/papers/member-symbol-audit-2026-08-13.md, family M6 (rank 3).
//
// ── WHAT THE AUDIT SAID, AND WHAT MEASUREMENT FOUND ────────────────────────
//
// The audit rates M6 reachable "**YES** for a non-virtual imported base
// indexer; the virtual sub-shape also needs #2837's ABI", and lists the crash
// at LlvmBackend_Assign.cpp:5319/5467 (`ensureIndexer{Getter,Setter}Fn`).
// Both halves of that framing are REFUTED by a two-module build:
//
//   * the NON-virtual sub-shape is already correct — #2414 exports a direct
//     `bz$<Class>$idxget$<arity>` stub and #2449 installs it
//     (tests/module/indexer_inherited_import_2449_test.cpp pins it);
//   * the VIRTUAL sub-shape crashes TODAY, without #2837's ABI, and it is not
//     indexer-specific: a plain named `virtual property` crashes identically,
//     through `ensurePropertyAccessorFn` — a site the audit's Appendix A marks
//     "ALREADY-CORRECT (guarded)" on the strength of #2387/#2400.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `Emitter::buildVtable`'s Pass D fill (LlvmBackend_Setup.cpp) resolves a
// virtual accessor slot with `classPropertyByName` / `classIndexerMatching`.
// Both walk `baseClassOf`, which crosses the module boundary, so the resolved
// implementation can be an IMPORTED `PropertySymbol` — which never carries an
// `ast::PropertyDecl`. The three body builders then had nothing to return:
//
//   * `ensureIndexerGetterFn` / `ensureIndexerSetterFn` returned nullptr;
//   * `ensurePropertyAccessorFn` fell back to `importedPropGetterFns_` /
//     `importedPropSetterFns_` (#2387/#2400) — but
//     `declareImportedProperty{Getters,Setters}` SKIP any property that has a
//     vtable slot (`if (propGetSlot_.count(ps)) continue;`), i.e. precisely
//     the virtual ones, so those maps are EMPTY for this shape.
//
// The slot kept its `ConstantPointerNull` initialiser and every dispatch
// called address 0x0.
//
// ── MEASURED, `bezelc -c lib.bzl -o lib.o` then `--import lib.o app.bzl` ───
//
//   spelling                                   pre-fix     post-fix / twin
//   d.P            (concrete derived receiver)  SIGSEGV/139       8
//   b.P            (base-typed reference)       SIGSEGV/139       8
//   b.P := 30; b.P (setter then getter)         SIGSEGV/139      35
//   i.P            (LOCAL interface, itable)    SIGSEGV/139       8
//   b[7]           (virtual INDEXER)            SIGSEGV/139     110
//   b[5] := 42; b[0]                            SIGSEGV/139     137
//
// Every row's character-identical single-module twin already answered the
// post-fix value, and each row below runs BOTH so the twin is the oracle
// rather than a number written down by hand. Also confirmed clean under
// `--sanitize=address`.
//
// ── CONTROLS (T7-T10) ──────────────────────────────────────────────────────
//
// The fix widens three `return nullptr`s. Each control pins a neighbour that
// must NOT move:
//   T7  a derived class that DOES override      -> its own local body, 99
//   T8  a virtual METHOD inherited the same way -> 42, already correct pre-fix
//       (a method HAS an ExternalLinkage `bz$<Class>$<M>` symbol; that
//        asymmetry is the whole defect)
//   T9  a two-level imported chain rooted at an ABSTRACT declaration — the
//       thunk must forward through the OVERRIDING imported class's vtable,
//       not the abstract root's (whose slot is legitimately null)
//   T10 #2837's `base[i]` reject on an imported virtual base indexer must stay
//       a clean compile error. The fix deliberately does NOT give #2837 what
//       it needs (a NON-virtual entry point): this thunk forwards to whatever
//       the base's own slot holds, which is the right answer for an inherited
//       accessor and the wrong one for `base[i]` past an override.

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

struct TempFile3190 {
    std::string path;
    explicit TempFile3190(std::string p) : path(std::move(p)) {}
    ~TempFile3190() { std::remove(path.c_str()); }
};

void writeSource3190(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// separate process, return its exit code. -1 on compile/link/run failure, -2
// if the dependency itself could not be produced (toolchain unavailable).
int compileImportAndRun3190(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3190_");
    TempFile3190 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3190 aObj((dir / (stem + "_a.o")).string());
    writeSource3190(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return -2;
    }
    TempFile3190 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3190 bExe((dir / stem).string());
    writeSource3190(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module twin runner: the SAME declarations with the module
// boundary removed.
int compileLocalAndRun3190(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3190l_");
    TempFile3190 aSrc((dir / (stem + ".bzl")).string());
    TempFile3190 aExe((dir / stem).string());
    writeSource3190(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Compile lib -> object, then app -> exe WITHOUT running, capturing stderr.
// `CompileResult` carries no diagnostic text, so a text assertion needs this.
bool compileImportCaptureErr3190(const std::string& libSrc,
                                 const std::string& appSrc, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3190c_");
    TempFile3190 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3190 aObj((dir / (stem + "_a.o")).string());
    writeSource3190(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return false;
    }
    TempFile3190 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3190 bExe((dir / (stem + "_b")).string());
    writeSource3190(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz3190_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        success = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                  deps)
                      .success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

// The dependency's declarations. `_v` starts at 3, so `P` reads 8 and `[i]`
// reads 103 + i until the setter writes.
//
// NOTE on #2452 (a LOCAL class deriving from an IMPORTED base does not run the
// base's field initializers): it does NOT confound these cases. The GET-only
// rows read the base instance through the SAME dispatch path in both builds,
// and every expected value below is taken from the single-module twin run in
// the same test — never written down by hand — so an initialisation
// difference would surface as a twin/import DISAGREEMENT rather than as a
// silently-passing assertion.
const char* kBody3190 =
    "public class VBase3190 "
    "  private _v: int := 3; "
    "  public virtual method M3190(): int "
    "    return 42; "
    "  end M3190; "
    "  public virtual property P: int "
    "    get => _v + 5; "
    "    set => _v := value; "
    "  end; "
    "  public virtual property [i: int]: int "
    "    get => _v + 100 + i; "
    "    set => _v := value - i; "
    "  end; "
    "end VBase3190; ";

std::string importedPair3190(const std::string& appBody) {
    return "module VApp3190; import VAccLib3190; " + appBody;
}

std::string localTwin3190(const std::string& appBody) {
    return "module VSingle3190; " + std::string(kBody3190) + appBody;
}

const char* kLib3190Head = "module VAccLib3190; ";

// Run one app body BOTH ways and require the two builds to agree — the twin is
// the oracle, `want` only pins that the agreed value is the spec-correct one.
void expectAgree3190(const std::string& appBody, int want) {
    int twin = compileLocalAndRun3190(localTwin3190(appBody));
    BEZEL_EXPECT_EQ(twin, want);
    int imported = compileImportAndRun3190(
        std::string(kLib3190Head) + kBody3190, importedPair3190(appBody));
    if (imported == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(imported, want);
    BEZEL_EXPECT_EQ(imported, twin);
}

}  // namespace

// ===========================================================================
// T1 — §12.5 / §23: a virtual property INHERITED (not overridden) from an
// imported base, read through the CONCRETE derived receiver. A virtual
// property read dispatches through the vtable even on a statically-typed
// receiver, so this is the shortest spelling that reaches the null slot.
// Pre-fix: SIGSEGV (139). Post-fix: 8, the twin's value.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           inherited_virtual_property_concrete_receiver_12_5_23) {
    expectAgree3190(
        "public class VD3190: VBase3190 "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    return d.P; "
        "  end Main; "
        "end C;",
        8);
}

// ===========================================================================
// T2 — the same accessor through a BASE-TYPED reference (§12.1 dispatch).
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           inherited_virtual_property_base_typed_reference_12_1) {
    expectAgree3190(
        "public class VD3190: VBase3190 "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    var b: VBase3190 := d; "
        "    return b.P; "
        "  end Main; "
        "end C;",
        8);
}

// ===========================================================================
// T3 — the SETTER slot (§23.3): write then read, so a passing run proves both
// accessors reach the SAME real storage on the base instance rather than one
// of them silently answering a constant.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           inherited_virtual_property_setter_then_getter_23_3) {
    expectAgree3190(
        "public class VD3190: VBase3190 "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    var b: VBase3190 := d; "
        "    b.P := 30; "
        "    return b.P; "
        "  end Main; "
        "end C;",
        35);
}

// ===========================================================================
// T4 — the ITABLE face (§11.2 / §13): a LOCAL interface obligation satisfied
// by the virtual property inherited from the imported base. This is the same
// slot-fill call (`ensurePropertyAccessorFn`) reached from Pass D's INTERFACE
// loop rather than its vtable loop, so it is a genuinely separate site.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           inherited_virtual_property_through_local_interface_11_2_13) {
    expectAgree3190(
        "public interface IP3190 "
        "  readonly property P: int; "
        "end IP3190; "
        "public class VD3190: VBase3190, IP3190 "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    var i: IP3190 := d; "
        "    return i.P; "
        "  end Main; "
        "end C;",
        8);
}

// ===========================================================================
// T5 — the INDEXER getter (§23.4), the site the audit named
// (LlvmBackend_Assign.cpp:5319). `_v (3) + 100 + 7` = 110.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           inherited_virtual_indexer_getter_23_4) {
    expectAgree3190(
        "public class VD3190: VBase3190 "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    var b: VBase3190 := d; "
        "    return b[7]; "
        "  end Main; "
        "end C;",
        110);
}

// ===========================================================================
// T6 — the INDEXER setter (§23.4), the site the audit named
// (LlvmBackend_Assign.cpp:5467). `b[5] := 42` writes `_v := 37`; `b[0]` then
// reads `37 + 100 + 0`.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           inherited_virtual_indexer_setter_then_getter_23_4) {
    expectAgree3190(
        "public class VD3190: VBase3190 "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    var b: VBase3190 := d; "
        "    b[5] := 42; "
        "    return b[0]; "
        "  end Main; "
        "end C;",
        137);
}

// ===========================================================================
// T7 — CONTROL (§12.5): the derived class OVERRIDES. Its own local
// `ast::PropertyDecl` must still win; the imported-forward thunk must not be
// consulted at all. Was correct pre-fix and must stay correct.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           local_override_still_wins_over_imported_base_12_5) {
    expectAgree3190(
        "public class VD3190: VBase3190 "
        "  private _w: int := 0; "
        "  public override property P: int "
        "    get => 99 + _w; "
        "    set => _w := value; "
        "  end; "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    var b: VBase3190 := d; "
        "    return b.P; "
        "  end Main; "
        "end C;",
        99);
}

// ===========================================================================
// T8 — CONTROL (§12.1): a virtual METHOD inherited the same way. This was
// ALREADY correct pre-fix, and that asymmetry is the defect's own diagnosis: a
// method carries an ExternalLinkage `bz$<Class>$<M>` symbol that `methodFns_`
// resolves, while `ensurePropertyAccessorFn` gives its accessor body
// InternalLinkage and no exporter emits an external wrapper for a virtual one.
// Pinned so a future ABI change cannot quietly take the method path with it.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           inherited_virtual_method_control_12_1) {
    expectAgree3190(
        "public class VD3190: VBase3190 "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    var b: VBase3190 := d; "
        "    return b.M3190(); "
        "  end Main; "
        "end C;",
        42);
}

// ===========================================================================
// T9 — CONTROL: a TWO-LEVEL imported chain rooted at an ABSTRACT declaration.
// `AAbs3190` declares `P`/`[i]` abstract (its own vtable slot is legitimately
// NULL — §12.5: the override fills it) and `BMid3190` overrides both; the
// LOCAL class derives from `BMid3190`. The forward must resolve through
// `vt$<BMid3190>`, the class that owns the resolved implementation — reading
// the abstract root's slot instead would reintroduce the identical crash.
// `11 + 2003` = 2014, truncated to 222 by the 8-bit exit status; the twin
// run is what makes that truncation harmless (both sides truncate alike).
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           two_level_imported_chain_abstract_root_12_5) {
    const std::string body =
        "public abstract class AAbs3190 "
        "  public abstract property P: int; "
        "  public virtual property [i: int]: int => i + 1000; "
        "end AAbs3190; "
        "public class BMid3190: AAbs3190 "
        "  public override property P: int => 11; "
        "  public override property [i: int]: int => i + 2000; "
        "end BMid3190; ";
    const std::string app =
        "public class VD3190: BMid3190 "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    var a: AAbs3190 := d; "
        "    return a.P + a[3]; "
        "  end Main; "
        "end C;";
    int twin = compileLocalAndRun3190("module VSingle3190B; " + body + app);
    BEZEL_EXPECT_EQ(twin, (11 + 2003) % 256);
    int imported = compileImportAndRun3190(
        "module VAccLib3190B; " + body,
        "module VApp3190B; import VAccLib3190B; " + app);
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T10 — the §12.3 OWNER-direct path for `base[i]` (#3231). Pre-#3231 this
// fix's `importedVirtualAccessorSlotFn` thunk forwarded through the OWNER's
// own vtable slot, which is the correct answer for an INHERITED accessor
// and the WRONG one for `base[i]` past a LOCAL override (§12.3 requires
// the OWNER's implementation, and the imported virtual entry dispatches
// to the override). #2832 rejected that spelling cleanly; #3231 widened
// the export gate (`emitExportedIndexerGetters` / `-Setters`) so a
// `bz$<Class>$idxget$<arity>` / `$idxset$<arity>` ExternalLinkage symbol
// is defined for VIRTUAL members too, and the importer side routes
// `base[i]` through it. The thunk route stays for non-`base` accesses
// (where vtable dispatch is correct). This test now asserts the post-
// #3231 behaviour: the program compiles, runs, and returns the OWNER's
// own implementation (`b[1]` on `VBase3190` is `_v + 100 + 1 = 104`)
// — NOT the override (`500 + 1 + _w = 501`).
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_accessor_3190,
           base_indexer_on_imported_virtual_base_uses_owner_body_12_3_3231) {
    const std::string body =
        "public class VD3190: VBase3190 "
        "  private _w: int := 0; "
        "  public override property [i: int]: int "
        "    get => 500 + i + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method ViaBase(i: int): int => base[i]; "
        "end VD3190; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new VD3190(); "
        "    return d.ViaBase(1); "
        "  end Main; "
        "end C;";
    // Single-module twin: same declarations, no module boundary. The
    // OWNER-direct path is what §12.3 mandates; the twin must agree.
    const std::string twinSrc =
        std::string("module Local3190T10; ") + kBody3190 + body;
    int twin = compileLocalAndRun3190(twinSrc);
    BEZEL_EXPECT_EQ(twin, 104);  // OWNER: _v + 100 + i = 3 + 100 + 1
    int imported = compileImportAndRun3190(
        std::string(kLib3190Head) + kBody3190,
        importedPair3190(body));
    if (imported == -2) return;
    // §12.3: base[i] must return the OWNER's body (104), NOT the override
    // (501) and NOT a silent 0.
    BEZEL_EXPECT_EQ(imported, 104);
    BEZEL_EXPECT_EQ(imported, twin);
}

}  // namespace bezel::module
