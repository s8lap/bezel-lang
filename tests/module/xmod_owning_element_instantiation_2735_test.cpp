// Spec-derived regression test for issue #2735 — the #2713 owning-element
// per-instantiation sweep does not reach CROSS-MODULE instantiations of an
// imported generic.
//
// stdlib §4.2 / spec §14.4 "Class-only restriction on owning generics" /
// §3.6 rule 2 "Per-instantiation evaluation for generics" / §2.8.3 rule 4
// (imported artifacts), v0.306.
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §14.4: the standard-library owning collections "restrict their element type
// T to class and interface types. Value types are rejected at the
// instantiation site. … compiler-enforced to reject the runtime-managed
// reference types (`string`, function values)." §3.6 rule 2 requires the
// property to be "evaluated at each instantiation, after substituting the type
// arguments", with an instantiation that violates it "rejected *at the
// instantiation site*".
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// #2713 satisfies that by recording an `OwningElementSite` in
// `Resolver::resolveType` while resolving THIS compilation's own type bodies,
// and re-checking it per instantiation in `checkOwningElementInstantiations`.
// An IMPORTED generic has no AST in the importing compilation at all —
// `ModuleLoader` builds its TypeRefs directly and `Symbol::decl` stays null for
// every imported symbol — so no site was ever recorded for it and Pass 2 had
// nothing to walk. The defining module never instantiates the type, so it could
// not catch it either. This is family F12 of the imported-symbol seam (#3152):
// the same boundary #3147 closed for the §28.5 owned-field check.
//
// ── MEASURED, on 6f252b1d, with real two-module builds ─────────────────────
//   (`bezelc -c dep.bzl -o dep.o` then
//    `bezelc --sanitize=address --import dep.o -o app app.bzl`)
//
// The issue reports "exits 0, silently wrong; no ASan abort was observed". It
// has MOVED: all three shapes below are a live `heap-use-after-free`, READ of
// size 8, freed by `bz_string_release`. The mechanism is #2713's — codegen's
// `elementPolicyFor()` falls back to the Value row for a runtime-managed
// element in the owning family, so the collection stores the pointer WITHOUT
// taking a reference while the `owned` argument transfer still releases the
// caller's, leaving a dangling slot that the read-back dereferences.
//
//   T1  imported `Holder2735<T> where T: class` carrying
//       `private owned _l: OwningLinkedList<T>`, instantiated `<string>`
//         -> ACCEPTED with no diagnostic at any severity; heap-use-after-free.
//   T3  the `OwningDictionary<K, V>` VALUE slot (slot 1, not slot 0)
//         -> ACCEPTED with no diagnostic; heap-use-after-free.
//   T4  a dependency with NO field at all, whose only surface is
//       `method Make(owned x: T): owned OwningLinkedList<T>`
//         -> ACCEPTED with no diagnostic; heap-use-after-free.
//
// ── THE FIX THIS PINS ──────────────────────────────────────────────────────
//
// `Resolver::recordImportedOwningElementObligations` synthesizes the missing
// sites from the imported type's reconstructed member surface at the
// instantiation site, so Pass 2 answers the cross-module case with the SAME
// predicate (`owningElementBadDesc`) and the SAME diagnostic. It needs NO new
// module metadata — which is what the issue declined to invent: the #3147
// channel (`sema::storageComponentType` over `tsym->members`, plus the sibling
// `SemaContext::importedReturnType` / `ParameterSymbol::declType` tables)
// already carries every fact the rule reads.
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
// Each reject row uses the ISSUE'S OWN REPRO SHAPE (the 20-iteration
// `Round`/read-back loop), not a minimal stand-in: a one-line
// `h.Put("x")` program never reads the dangling slot back and so is clean on
// BOTH sides of the fix. FAIL-WITHOUT was proved mechanically by reverting
// both source files, rebuilding, and re-running all five rows.
//
// T2 is the differential control: the same declarations in ONE module were
// already rejected by #2713, before AND after, so the module boundary is
// proved to be the whole difference. T5 is the anti-false-reject control and
// the only row that RUNS: it exercises the three shapes the fix must NOT have
// swept in — a class-typed element (legal), `OwningBuffer<int>` (§14.4's
// explicit value-admitting exception), and the borrowing `Array<string>` —
// in one program whose exact value (12) is asserted.

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

namespace fs = std::filesystem;

struct TempFile2735 {
    std::string path;
    explicit TempFile2735(std::string p) : path(std::move(p)) {}
    ~TempFile2735() { std::remove(path.c_str()); }
};

void writeSource2735(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled2735 {
    bool depOk = true;    // false => dependency toolchain unavailable; skip
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so BOTH the driver's
// std::cerr diagnostics and any llvm::errs() text are captured (the shape
// tests/module/xmod_imported_symbol_residues_3147_test.cpp established).
template <typename Fn>
std::string captureStderr2735(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz2735_err_" + unique_stem("") + ".txt")).string();
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

// Build `libSrc` to an object, then compile `appSrc` against it.
Compiled2735 compileImport2735(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2735_");
    TempFile2735 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2735 aObj((dir / (stem + "_a.o")).string());
    writeSource2735(aSrc.path, libSrc);
    Compiled2735 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile2735 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2735 bExe((dir / stem).string());
    writeSource2735(bSrc.path, appSrc);
    r.err = captureStderr2735([&] {
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

// The one-module twin of the above — same capture, no dependency.
Compiled2735 compileLocal2735(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2735l_");
    TempFile2735 aSrc((dir / (stem + ".bzl")).string());
    TempFile2735 aExe((dir / stem).string());
    writeSource2735(aSrc.path, src);
    Compiled2735 r;
    r.err = captureStderr2735([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
//
// `compileFileForTest` / `runProgramForTest` (#4532) are what wire this
// hand-rolled runner into `gate.sh --full`: with
// BEZEL_AOT_FORCE_SANITIZE=address the importer's code is built under
// generated-code ASan and run with the sweep's own
// `detect_leaks=0:abort_on_error=0` options, so a redzone error (this issue's
// heap-use-after-free) fails the row loudly while the row keeps asserting the
// VALUE it exists to assert. Without the sweep it is a plain build and a plain
// `std::system`, byte-for-byte as before.
int buildImportAndRun2735(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz2735r_");
    TempFile2735 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile2735 libObjF((dir / (id + "_lib.o")).string());
    TempFile2735 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile2735 mainExeF((dir / id).string());
    writeSource2735(libSrcF.path, libSrc);
    writeSource2735(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return -1;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The dependency. One module carrying every shape the rows exercise.
// ---------------------------------------------------------------------------
//
//   Holder2735<T>  owning collection in a FIELD, owned slot 0     (T1)
//   Reg2735<V>     OwningDictionary VALUE slot, owned slot 1      (T3)
//   Fac2735<T>     NO field — the obligation is on a RETURN type  (T4)
//   Taker2735<T>   NO field — the obligation is on a PARAMETER    (T7)
//   Loose2735<T>   no `where T: class`, so a primitive may bind   (T6)
//   Buf2735<T>     §14.4's OwningBuffer exception — must ADMIT int (T5)
//   Bor2735<T>     the borrowing family — must ADMIT string       (T5)
//   Item2735       a plain class, the legal element               (T5)
const char* kDep2735 =
    "module Dep2735; "
    "public class Item2735 "
    "  public readonly property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item2735; "
    "public class Holder2735<T> where T: class "
    "  private owned _l: OwningLinkedList<T> := new OwningLinkedList<T>(); "
    "  public method Put(owned x: T): int "
    "    _l.AddLast(owned x); "
    "    return _l.Count; "
    "  end Put; "
    "  public method Head(): T? "
    "    return _l.First; "
    "  end Head; "
    "end Holder2735; "
    "public class Reg2735<V> where V: class "
    "  private owned _d: OwningDictionary<int, V> := "
    "      new OwningDictionary<int, V>(); "
    "  public method Put(k: int, owned v: V): int "
    "    _d.Add(k, owned v); "
    "    return _d.Count; "
    "  end Put; "
    "  public method Get(k: int): V? "
    "    return _d.GetOrNull(k); "
    "  end Get; "
    "end Reg2735; "
    "public class Fac2735<T> where T: class "
    "  public method Make(owned x: T): owned OwningLinkedList<T> "
    "    var l := new OwningLinkedList<T>(); "
    "    l.AddLast(owned x); "
    "    return l; "
    "  end Make; "
    "end Fac2735; "
    "public class Taker2735<T> where T: class "
    "  public method Take(l: OwningLinkedList<T>): int "
    "    return l.Count; "
    "  end Take; "
    "end Taker2735; "
    "public class Loose2735<T> "
    "  private owned _l: OwningLinkedList<T> := new OwningLinkedList<T>(); "
    "  public method N(): int "
    "    return _l.Count; "
    "  end N; "
    "end Loose2735; "
    "public class Buf2735<T> "
    "  private owned _b: OwningBuffer<T> := new OwningBuffer<T>(4); "
    "  public method Cap(): int "
    "    return _b.Length; "
    "  end Cap; "
    "end Buf2735; "
    "public class Bor2735<T> "
    "  private owned _a: Array<T> := new Array<T>(); "
    "  public method N(): int "
    "    return _a.Count; "
    "  end N; "
    // The PARAMETER-arm accept control: a BORROWING collection over T in a
    // method formal must stay admitted at `Bor2735<string>` (T5), while
    // Taker2735's owning one is rejected at `Taker2735<string>` (T7).
    "  public method Absorb(a: Array<T>): int "
    "    return a.Count; "
    "  end Absorb; "
    "end Bor2735; ";

// The §14.4 reject, in the wording `checkOwningElementInstantiations` emits.
// Matching a SPECIFIC substring (not `failedAtCompileTimeWithRealLocation`,
// which would answer true on both sides of a crash-shaped fix) is what makes
// these rows an oracle rather than a smoke test.
const char* kRejectHead2735 =
    "requires a class or interface element type";
const char* kRejectPerInstantiation2735 =
    "used as its owning-collection element type, is bound to";
const char* kRejectString2735 = "`string` (a runtime-managed reference type)";
const char* kRejectPrimitive2735 = "a primitive value type";

// The issue's repro shape, parameterised by the dependency type under test.
// The 20-iteration read-back loop is load-bearing: the dangling slot must
// actually be DEREFERENCED for the pre-fix behaviour to be the measured
// heap-use-after-free rather than a clean run.
std::string roundLoop2735(const std::string& moduleName,
                          const std::string& roundBody) {
    return "module " + moduleName + "; import Dep2735; " +
        "class C2735 "
        "  public static method Round(i: int): int " + roundBody +
        "  end Round; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 1..20 do "
        "      total := total + C2735.Round(j); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C2735;";
}

bool mentions2735(const Compiled2735& c, const char* needle) {
    return c.err.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — ★ THE BUG. An imported generic whose FIELD is an owning collection over
// the type's own parameter, instantiated at `string`. Pre-fix: compiled clean,
// heap-use-after-free at run time. Post-fix: rejected at the instantiation
// site, naming the container, the parameter and `string`.
// // per stdlib §4.2 / §14.4 / §3.6 rule 2, v0.306
// ===========================================================================
BEZEL_TEST(xmod_owning_element_instantiation_2735,
           imported_owning_linked_list_field_rejects_string_element) {
    auto r = compileImport2735(
        kDep2735,
        roundLoop2735("App2735T1",
                      "    using var h := new Holder2735<string>(); "
                      "    var n := h.Put($\"value-{i}\"); "
                      "    var s := h.Head(); "
                      "    if s <> null then "
                      "      return s!.Length; "
                      "    end; "
                      "    return 0; "));
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectHead2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectPerInstantiation2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectString2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, "OwningLinkedList"));
    BEZEL_EXPECT_TRUE(mentions2735(r, "Holder2735"));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// T2 — ★ THE DIFFERENTIAL CONTROL. The same declarations in ONE module were
// already rejected by #2713's Pass 2: green before AND after, so the module
// boundary is proved to be the whole difference.
// // per §14.4 / §3.6 rule 2 (#2713)
// ===========================================================================
BEZEL_TEST(xmod_owning_element_instantiation_2735,
           local_owning_linked_list_field_rejects_string_element_control) {
    auto r = compileLocal2735(
        "module Local2735; "
        "public class HolderL2735<T> where T: class "
        "  private owned _l: OwningLinkedList<T> := new OwningLinkedList<T>(); "
        "  public method Put(owned x: T): int "
        "    _l.AddLast(owned x); "
        "    return _l.Count; "
        "  end Put; "
        "end HolderL2735; "
        "class C2735L "
        "  public static method Main(): int "
        "    using var h := new HolderL2735<string>(); "
        "    return h.Put(\"x\"); "
        "  end Main; "
        "end C2735L;");
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectHead2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectString2735));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// T3 — the `OwningDictionary<K, V>` VALUE slot (slot 1, not slot 0). Pins that
// the cross-module walk asks the same two-entry slot table the declaration
// site does, rather than assuming slot 0. §28.11: the KEY is borrowed and
// value-like by design and is checked by `checkHashContainerKeys` instead.
// Pre-fix: compiled clean, heap-use-after-free.
// // per stdlib §4.2 / §14.4 / §28.11
// ===========================================================================
BEZEL_TEST(xmod_owning_element_instantiation_2735,
           imported_owning_dictionary_value_slot_rejects_string_element) {
    auto r = compileImport2735(
        kDep2735,
        roundLoop2735("App2735T3",
                      "    using var g := new Reg2735<string>(); "
                      "    var n := g.Put(i, $\"value-{i}\"); "
                      "    var s := g.Get(i); "
                      "    if s <> null then "
                      "      return s!.Length; "
                      "    end; "
                      "    return 0; "));
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectHead2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectString2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, "OwningDictionary"));
    BEZEL_EXPECT_TRUE(mentions2735(r, "Reg2735"));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// T4 — the dependency has NO field at all: its only surface is
// `method Make(owned x: T): owned OwningLinkedList<T>`. Pins the method
// SIGNATURE arm of the walk (`SemaContext::importedReturnType`), which the
// field-only shape of #3147 would not have covered. Pre-fix: compiled clean,
// heap-use-after-free. // per stdlib §4.2 / §14.4 / §3.6 rule 2
// ===========================================================================
BEZEL_TEST(xmod_owning_element_instantiation_2735,
           imported_owning_collection_return_type_rejects_string_element) {
    auto r = compileImport2735(
        kDep2735,
        roundLoop2735("App2735T4",
                      "    using var f := new Fac2735<string>(); "
                      "    using var l := f.Make($\"value-{i}\"); "
                      "    var h := l.First; "
                      "    if h <> null then "
                      "      return h!.Length; "
                      "    end; "
                      "    return 0; "));
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectHead2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectString2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, "Fac2735"));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// T5 — ★ THE ANTI-FALSE-REJECT CONTROL, and the only row that RUNS. Three
// shapes the widened rejection must NOT sweep in, in one program:
//   * `Holder2735<Item2735>` — a class element, §14.4's admitted case;
//   * `Buf2735<int>` — §14.4 names `OwningBuffer<T>` an explicit EXCEPTION
//     that admits value-typed T (it carries its own §28.11 move-only
//     obligation instead, via owningBufferSpanMoveOnlySites_);
//   * `Bor2735<string>` — the BORROWING family (`Array<T>`) is outside the
//     rule entirely, in a FIELD and in a method PARAMETER (`Absorb(a:
//     Array<T>)`), so it is also the parameter arm's accept control.
// The oracle is the exact value, 7 + 1 + 4 + 0 = 12 — not "it compiled" — so a
// wrong answer or a redzone error under `--full` fails the row.
// // per §14.4 (owning-type exceptions) / stdlib §4.2, §4.14
// ===========================================================================
BEZEL_TEST(xmod_owning_element_instantiation_2735,
           imported_class_element_and_owning_buffer_and_array_stay_admitted) {
    int rc = buildImportAndRun2735(
        kDep2735,
        "module App2735T5; import Dep2735; "
        "class C2735A "
        "  public static method Main(): int "
        "    using var h := new Holder2735<Item2735>(); "
        "    var n := h.Put(new Item2735(7)); "
        "    var x := h.Head(); "
        "    using var b := new Buf2735<int>(); "
        "    using var r := new Bor2735<string>(); "
        "    if x <> null then "
        "      return x!.V + n + b.Cap() + r.N(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C2735A;");
    if (rc == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(12, rc);
}

// ===========================================================================
// T6 — a VALUE type, not just `string`. `Loose2735<T>` declares no
// `where T: class`, so `int` reaches the element slot and §14.4's "Value types
// are rejected at the instantiation site" is what must fire. Pins that the
// cross-module path calls the full `owningElementBadDesc` predicate and not a
// `string`-only special case. Pre-fix: compiled clean.
// // per §14.4 (value types rejected at the instantiation site)
// ===========================================================================
BEZEL_TEST(xmod_owning_element_instantiation_2735,
           imported_owning_collection_rejects_primitive_value_element) {
    auto r = compileImport2735(
        kDep2735,
        "module App2735T6; import Dep2735; "
        "class C2735B "
        "  public static method Main(): int "
        "    using var l := new Loose2735<int>(); "
        "    return l.N(); "
        "  end Main; "
        "end C2735B;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectHead2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectPrimitive2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, "Loose2735"));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// T7 — the obligation is on a method PARAMETER, with no field and no owning
// return type. Pins the parameter arm, which reads the named both-sources
// accessor `formalParamResolvedType`: an imported member's formal carries no
// syntactic `declType` at all (ModuleLoader stashes the resolved TypeRef on
// `ParameterSymbol::resolvedDeclType` instead), and reading `declType` alone
// was MEASURED to leave this arm dead for every import — the same false
// ACCEPT #2373 and #3194 each fixed at their own site. The EXPORTER already
// applies §14.4 to this exact signature via #2713's Pass 2 over its own
// instantiations, so admitting it across the boundary was pure asymmetry.
// Pre-fix: compiled clean. // per stdlib §4.2 / §14.4 / §2.8.2
// ===========================================================================
BEZEL_TEST(xmod_owning_element_instantiation_2735,
           imported_owning_collection_parameter_type_rejects_string_element) {
    auto r = compileImport2735(
        kDep2735,
        "module App2735T7; import Dep2735; "
        "class C2735C "
        "  public static method Main(): int "
        "    using var t := new Taker2735<string>(); "
        "    return 0; "
        "  end Main; "
        "end C2735C;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectHead2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, kRejectString2735));
    BEZEL_EXPECT_TRUE(mentions2735(r, "Taker2735"));
    BEZEL_EXPECT_FALSE(r.success);
}

}  // namespace bezel::module
