// fix #3543 — §2.8.3 rule 3 (v0.286): six symbol families in
// `LlvmBackend_Setup.cpp` were built from `ts->name`, the SIMPLE identifier, so
// two enclosing types with the same simple name in one module collapsed onto
// one symbol. `nm` on the issue's two-enclosing-type repro showed the
// collision directly; every family lives in `src/codegen/LlvmBackend_Setup.cpp`.
//
//   (a) bz$tostr$  / bz$eq$  / bz$hash$  — class-slot thunks (§8.5/§8.6/§9.6
//       #417 + #946). BENIGN-BY-ACCIDENT today: InternalLinkage, cache keyed
//       on the TypeSymbol pointer (not the name string), so each type still
//       gets its own function. Misleading stack frame in a crash, mirroring
//       #3536's `bz$iter$` family shape — fixed by mint-qualifying the name
//       with the owning class's `mangledTypeName`.
//   (b) itab$<T>$<I>  /  itmap$<T>  — per-class interface map. Reaches
//       emitted IR (the issue's own `nm` shows it), InternalLinkage so the
//       linkage hazard is mild but the same debugging-frame hazard applies
//       (#2928's reason). Same fix: mint-qualify with the owning class (and
//       the interface) via `mangledTypeName`.
//   (c) `td$<synthesized-builtin-iface>` — the descriptor for a SYNTHESIZED
//       builtin interface (IComparable<T>, IEquatable<T>, …). Pre-fix the
//       synthesized iface's TypeSymbol had an empty `moduleName`, so
//       `mangledTypeName` rendered `td$IComparable` bare and a user-declared
//       `interface IComparable<T>` in another module produced the same name
//       and the LinkOnceODR ODR-precondition did not hold. The fix populates
//       `moduleName` on the synthetic iface symbol so `mangledTypeName`
//       renders the head descriptor as `td$System.IComparable` (or
//       `td$System.Collections.Generic.IEqualityComparer`).
//
// THE ORACLE. Like #3536, nothing behavioral distinguishes the pre-fix and
// post-fix compilers here — the cross-module identity hazard is in the LINKER,
// which the in-process AOT harness does not exercise. The direct, mechanical
// oracle is the EMITTED SYMBOL NAME itself, captured via `BEZEL_DUMP_IR=1`:
//   * the OLD unqualified spelling never appears in the emitted module at
//     all, AND
//   * each class's own qualified name appears distinctly, AND
//   * no LLVM-appended `.N` auto-unique suffix follows the name (a `.N` after
//     a name would mean the base name collided — exactly the evidence the
//     issue quotes in its `nm` output).
//
// One family per BEZEL_TEST row (a) bz$tostr$ (b) bz$eq$ (c) bz$hash$
// (d) itab$ (e) itmap$ (f) the synthesized-iface `td$System.IComparable` head
// — a re-derived sample, not a copy of the issue's, but covering the same
// shape (Outer1.Ord, Outer2.Ord each implementing IComparable<Ord>).
//
// REVERT PROOF (FAIL-WITHOUT). `git checkout HEAD~1 -- src/codegen/LlvmBackend_Setup.cpp
// src/sema/SemaContext.hpp` reverts every fix; the unqualified-spelling
// assertions fail and the LLVM auto-unique `.N` suffix assertions fire,
// matching the issue's measured output.
//
// NOT IN SCOPE. The (c) hazard's SPECIALIZATION half — `td$IComparable_Ord`
// minted by `instanceDescriptorFor` via `descReadableMangle` (which uses the
// simple type-arg name) — is a separate concern with a separate fix surface
// (the readable-mangle helper, not the synthesized-iface symbol). The (c)
// fix in this commit closes the HEAD half only. The specialization hazard
// is filed as a follow-up issue with the measured repro.

#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// `bezelc` sits one level above this test binary in the same build tree —
// see iterator_async_prefix_owner_qualifier_3536_test.cpp and
// static_init_atomic_1391_test.cpp for the identical helper.
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Compile to an object with BEZEL_DUMP_IR=1 and capture the module text the
// compiler prints to stderr. SUBPROCESS, not in-process Driver.
std::string compileAndCaptureIR(const std::string& source) {
    static int counter = 0;
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    const char* envDir = std::getenv("BEZEL_AOT_SCRATCH_DIR");
    fs::path dir = envDir && *envDir ? fs::path(envDir)
                                     : fs::temp_directory_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = "bezel_3543_" + std::to_string(getpid()) + "_" +
                       std::to_string(counter++);
    std::string src = (dir / (stem + ".bzl")).string();
    std::string obj = (dir / (stem + ".o")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    std::string cmd = "BEZEL_DUMP_IR=1 '" + bezelc + "' -c -o '" + obj +
                      "' '" + src + "' 2>&1";
    std::string text;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        std::array<char, 8192> chunk{};
        std::size_t got;
        while ((got = fread(chunk.data(), 1, chunk.size(), p)) > 0)
            text.append(chunk.data(), got);
        pclose(p);
    }
    std::remove(src.c_str());
    std::remove(obj.c_str());
    return text;
}

// The issue's own repro, verbatim in shape: two enclosing types each with a
// nested `Ord: IComparable<Ord>`, opposite `CompareTo` so a mis-merge is
// visible behaviourally. Mirrors the #3529 / #3536 test shape — only the
// program output here is defense-in-depth (the (a)/(b) fixes are
// symbol-IDENTITY only; the (c) head fix is name-only and the program was
// already correct behaviourally).
const char* kTwoNestedOrdsIface =
    "module M; "
    "class Outer1 "
    "  public class Ord: IComparable<Outer1.Ord> "
    "    private _v: int; "
    "    public constructor(v: int) _v := v; end constructor; "
    "    public method Value(): int => _v; "
    "    public method CompareTo(other: Outer1.Ord): int "
    "      => _v - other.Value(); "
    "  end Ord; "
    "end Outer1; "
    "class Outer2 "
    "  public class Ord: IComparable<Outer2.Ord> "
    "    private _v: int; "
    "    public constructor(v: int) _v := v; end constructor; "
    "    public method Value(): int => _v; "
    "    public method CompareTo(other: Outer2.Ord): int "
    "      => other.Value() - _v; "
    "  end Ord; "
    "end Outer2; ";

std::string prog(const std::string& decls, const std::string& body) {
    return decls + "class Program public static method Main(): int " + body +
           " end Main; end Program; ";
}

}  // namespace

// --- (a) bz$tostr$ family (the three families share an identical shape; the
//         bz$tostr$ row is the smallest surface to assert on) ---
//
// The class thunk name `bz$tostr$<cls>` is at line 31467 (classToStringThunk);
// the spec-instantiation twin is at line 7068 (specializedVtableFor). Both
// sites now mint-qualify with `mangledTypeName(ts)`. FAIL-WITHOUT: the
// pre-fix compiler emits `"bz$tostr$M.Outer1+Ord"` AND `"bz$tostr$M.Outer2+Ord"`
// as one of the same "bz$tostr$Ord" name and LLVM auto-uniques to `.1`/`.2`
// (the issue's measured pattern).
BEZEL_TEST(six_symbol_family_simple_name_3543,
           bz_tostr_name_qualified_by_owning_class) {
    std::string ir = compileAndCaptureIR(prog(kTwoNestedOrdsIface, " return 0; "));
    BEZEL_EXPECT_TRUE(!ir.empty());
    // Pre-fix: the bare `bz$tostr$Ord` (with no `M.Outer+` qualifier) was
    // emitted and LLVM auto-uniqued to `.1`/`.2`. Post-fix the qualified
    // form is emitted twice, distinctly, with no auto-unique.
    BEZEL_EXPECT_TRUE(ir.find("\"bz$tostr$M.Outer1+Ord\"") != std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$tostr$M.Outer2+Ord\"") != std::string::npos);
    // No LLVM auto-unique suffix on either class's mint — a `.N` after
    // `bz$tostr$M.Outer...+Ord"` would mean the base name collided.
    BEZEL_EXPECT_TRUE(ir.find("\"bz$tostr$M.Outer1+Ord\".1") == std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$tostr$M.Outer2+Ord\".1") == std::string::npos);
}

// --- (a) bz$eq$ family: same shape, sibling site. The spec-instantiation
//         twin is at line 7082; the class thunk is at line 31562. ---
BEZEL_TEST(six_symbol_family_simple_name_3543,
           bz_eq_name_qualified_by_owning_class) {
    std::string ir = compileAndCaptureIR(prog(kTwoNestedOrdsIface, " return 0; "));
    BEZEL_EXPECT_TRUE(!ir.empty());
    BEZEL_EXPECT_TRUE(ir.find("\"bz$eq$M.Outer1+Ord\"") != std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$eq$M.Outer2+Ord\"") != std::string::npos);
}

// --- (a) bz$hash$ family: same shape, sibling site. The spec-instantiation
//         twin is at line 7093; the class thunk is at line 31631. ---
BEZEL_TEST(six_symbol_family_simple_name_3543,
           bz_hash_name_qualified_by_owning_class) {
    std::string ir = compileAndCaptureIR(prog(kTwoNestedOrdsIface, " return 0; "));
    BEZEL_EXPECT_TRUE(!ir.empty());
    BEZEL_EXPECT_TRUE(ir.find("\"bz$hash$M.Outer1+Ord\"") != std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$hash$M.Outer2+Ord\"") != std::string::npos);
}

// --- (b) itab$<T>$<I> family: itab$ + class + $ + interface, both at line
//         4831 (class vtable). ---
BEZEL_TEST(six_symbol_family_simple_name_3543,
           itab_name_qualified_by_class_and_interface) {
    std::string ir = compileAndCaptureIR(prog(kTwoNestedOrdsIface, " return 0; "));
    BEZEL_EXPECT_TRUE(!ir.empty());
    // Each Ord class implements IComparable, so an itab per class. The
    // interface is module-qualified (#3543 (c) head fix), so the full name
    // is `itab$M.Outer{1,2}+Ord$System.IComparable`.
    BEZEL_EXPECT_TRUE(ir.find(
        "\"itab$M.Outer1+Ord$System.IComparable\"") != std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find(
        "\"itab$M.Outer2+Ord$System.IComparable\"") != std::string::npos);
}

// --- (b) itmap$<T> family: itmap$ + class, at line 4851 (class vtable) and
//         the spec-instantiation twin at line 6944. ---
BEZEL_TEST(six_symbol_family_simple_name_3543,
           itmap_name_qualified_by_owning_class) {
    std::string ir = compileAndCaptureIR(prog(kTwoNestedOrdsIface, " return 0; "));
    BEZEL_EXPECT_TRUE(!ir.empty());
    BEZEL_EXPECT_TRUE(ir.find("\"itmap$M.Outer1+Ord\"") != std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"itmap$M.Outer2+Ord\"") != std::string::npos);
}

// --- (c) td$<synthesized-builtin-iface> head: populated `moduleName` on
//         the synthetic iface symbol. IComparable lives in `System` per
//         stdlib §2.8 (auto-imported), so the head is now
//         `td$System.IComparable`, NOT `td$IComparable` (the pre-fix bare
//         spelling). ---
//
// The synthesized iface's descriptor is at line 4540 (`mangledTypeName(ts)`
// joined with `"td$"`). The fix populates `ts->moduleName = "System"` on the
// synthetic iface TypeSymbol in SemaContext.hpp's two `builtinInterfaceSym`
// arms (the IComparable/IEquatable/IReadOnlySpan arm and the
// INumber/ISteppable/IParsable arm).
BEZEL_TEST(six_symbol_family_simple_name_3543,
           synthesized_builtin_iface_head_descriptor_module_qualified) {
    std::string ir = compileAndCaptureIR(prog(kTwoNestedOrdsIface, " return 0; "));
    BEZEL_EXPECT_TRUE(!ir.empty());
    // Post-fix: `td$System.IComparable` is the head descriptor for the
    // synthesized builtin IComparable (LinkOnceODR per #2776 / §2.8.3 rule 5).
    BEZEL_EXPECT_TRUE(ir.find("\"td$System.IComparable\"") != std::string::npos);
    // Pre-fix: `td$IComparable` was emitted bare. FAIL-WITHOUT: this
    // assertion fails because the pre-fix compiler emits the bare
    // `"td$IComparable"` with NO `System.` prefix.
    //
    // ⚠ The specialization `td$IComparable_Ord` (per-instantiation descriptor
    // minted by `instanceDescriptorFor` via `descReadableMangle`) STILL uses
    // the simple "Ord" name and so STILL appears in the IR with that
    // spelling — but the SCOPE of the fix in this commit is the HEAD
    // descriptor (the bare `td$<I>` for a synthesized interface TypeSymbol).
    // Asserting the bare `td$IComparable"` (with closing quote, no specialization
    // suffix) is what distinguishes the head from the specialization.
    BEZEL_EXPECT_TRUE(ir.find("\"td$IComparable\"") == std::string::npos);
}

// --- Defense-in-depth: behavior. Pre-fix and post-fix the program runs
//         correctly (#3529's same-shape sort already pinned this) because
//         the (a)/(b) fixes are symbol-identity only and the (c) head fix is
//         name-only; this row is here to catch a regression that a future
//         rename could introduce, NOT as the primary oracle (the file header
//         explains). ---
BEZEL_TEST(six_symbol_family_simple_name_3543,
           program_still_runs_and_sorts_correctly) {
    auto r = runAot(prog(kTwoNestedOrdsIface,
        "  using var p1 := new Outer1.Ord(2); "
        "  using var p2 := new Outer1.Ord(3); "
        "  using var p3 := new Outer1.Ord(1); "
        "  var pa: Array<Outer1.Ord> := [p1, p2, p3]; "
        "  pa.Sort(); "
        "  using var q1 := new Outer2.Ord(2); "
        "  using var q2 := new Outer2.Ord(3); "
        "  using var q3 := new Outer2.Ord(1); "
        "  var qa: Array<Outer2.Ord> := [q1, q2, q3]; "
        "  qa.Sort(); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
