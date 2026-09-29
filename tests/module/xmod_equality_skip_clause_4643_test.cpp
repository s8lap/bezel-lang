// §9.2 "The `equality skip` clause" (v0.307) — the CROSS-MODULE half, the
// follow-up to fix #4643 (umbrella #3152, the imported-symbol classification
// seam).
//
// §9.2 states the obligation in one sentence: "Adding or removing
// `equality skip` on a public type's member is a breaking change ([§3.6] rule
// 3): it flips comparison results and hash values for clients, and can flip
// the auto-hashable/hashable derived properties; the artifact interface
// ([§2.8.2]) carries the clause exactly as it carries `serialized` clauses."
//
// #4643 landed the clause riding `ast::FieldDecl::equality` /
// `ast::PropertyDecl::equality` / `ast::Param::equality` and nothing else, so
// every consumer — sema's two auto-equality membership predicates and
// codegen's `=` / `GetHashCode` synthesis — recovered it by casting
// `Symbol::decl`, which `ModuleLoader` never sets for an imported member. The
// clause was therefore INVISIBLE across a module boundary, in both directions
// the spec names:
//
//   * PARTICIPATION. MEASURED on the pre-fix compiler, T1/T2's exact program:
//     `new Pt(1,10) = new Pt(1,20)` (where `Cache` is `equality skip`)
//     printed `True` inside the declaring module and `False` at the import,
//     with the identical split on `GetHashCode` — a silent wrong ANSWER, and
//     a §8.3 hashing-coherence split between the two sides of one boundary.
//   * THE ELIGIBILITY GATE. The clause's stated purpose is a rescue: "a member
//     whose type is inadmissible ... does not disqualify the record when
//     marked `equality skip`". With the clause invisible, the importer
//     re-derives the §9.2 membership computation WITHOUT the rescue and
//     classifies the record as not auto-hashable — so T3's cross-module `=`
//     was a COMPILE ERROR ("`=` is not available on record `Holder`: it is
//     not auto-hashable") on a program the declaring module accepts.
//
// THE FIX carries the clause on the symbol (`MemberSymbol::equalitySkip`),
// filled by `SymbolCollection` from the three storage-bearing AST hosts and by
// `ModuleLoader` from the new `MetaMember::equalitySkip` wire bit — the
// `builtinExceptionBase` (#2950) / `serializedSkip` (#3140) template — and
// routes every consumer through the one `sema::memberEqualitySkip` predicate.
//
// ORACLE DISCIPLINE. The pre-fix behaviour is a WRONG VALUE (T1/T2/T4) or a
// spurious REJECT (T3), never a crash, so every row asserts the exact §9.2
// verdict as a distinct prime; a row that fails to compile returns -1 and a
// row whose dependency toolchain is unavailable returns -2 and skips, so
// neither can be mistaken for a pass. T4 is the CONTROL in the opposite
// direction: the same record WITHOUT the clause must still compare its second
// component across the boundary, so a fix that blanket-skipped everything
// (the other way to make T1 green) leaves T4 red.

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

struct TempFile4643 {
    std::string path;
    explicit TempFile4643(std::string p) : path(std::move(p)) {}
    ~TempFile4643() { std::remove(path.c_str()); }
};

void writeSource4643(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Mirrors #2478's `compileImportAndRun2478` (tests/module/
// xmod_auto_property_storage_layout_2478_test.cpp) exactly: -2 = the
// dependency toolchain is unavailable (skip), -1 = the IMPORTING compilation
// failed, otherwise the program's WEXITSTATUS.
int compileImportAndRun4643(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4643_");
    TempFile4643 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4643 aObj((dir / (stem + "_a.o")).string());
    writeSource4643(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile4643 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4643 bExe((dir / stem).string());
    writeSource4643(bSrc.path, appSrc);
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

// The exported library for T1/T2/T4: one positional record carrying the
// clause on its SECOND component, one that does not, and a class so the
// rescue row has an inadmissible component type to rescue.
constexpr const char* kLib4643 =
    "module Lib4643; "
    "public class Box4643 public V: int := 0; end Box4643; "
    "public record Pt4643(X: int, Cache: int equality skip); "
    "public record Plain4643(X: int, Cache: int); "
    "public record Holder4643(Key: int, Cache: Box4643 equality skip);";

}  // namespace

// ===========================================================================
// T1 — PARTICIPATION, the discovering repro. Two values of an IMPORTED
// positional record differ ONLY in the `equality skip` component. §9.2: the
// component "does not participate in the generated `=`/`<>`", so they are
// equal. Pre-fix the importer compared it and answered `False` (41 -> 0),
// while the declaring module answered `True`.
// ===========================================================================
BEZEL_TEST(xmod_equality_skip_clause_4643,
           imported_positional_skip_component_not_compared_9_2) {
    int r = compileImportAndRun4643(
        kLib4643,
        "module App4643T1; import Lib4643; "
        "public class MainT1 "
        "  public static method Main(): int "
        "    var a := new Pt4643(1, 10); "
        "    var b := new Pt4643(1, 20); "
        "    if a = b then return 41; end; "
        "    return 0; "
        "  end Main; "
        "end MainT1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 41);
}

// ===========================================================================
// T2 — the §8.3 hashing-coherence half, pinned SEPARATELY because it is a
// separate synthesis site (`emitRecordHashCode`, LlvmBackend_String.cpp) from
// T1's (`emitStructEquality`, LlvmBackend_Layout.cpp): a fix reaching only one
// of them leaves the other red. §9.2: "The generated `=` and the generated
// `GetHashCode` are computed over the *same* reduced member set ... No
// configuration exists in which a member is hashed but not compared, or
// compared but not hashed." Pre-fix the importer hashed the skipped component
// and the two hashes differed (43 -> 0).
// ===========================================================================
BEZEL_TEST(xmod_equality_skip_clause_4643,
           imported_positional_skip_component_not_hashed_9_2_8_3) {
    int r = compileImportAndRun4643(
        kLib4643,
        "module App4643T2; import Lib4643; "
        "public class MainT2 "
        "  public static method Main(): int "
        "    var a := new Pt4643(1, 10); "
        "    var b := new Pt4643(1, 20); "
        "    if a.GetHashCode() = b.GetHashCode() then return 43; end; "
        "    return 0; "
        "  end Main; "
        "end MainT2;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 43);
}

// ===========================================================================
// T3 — THE ELIGIBILITY GATE (the clause's stated rescue purpose). The skipped
// component is CLASS-typed, i.e. inadmissible under §9.2's component rule, so
// without the clause the record is not auto-hashable at all. §9.2: the member
// "is exempt from the admissible-component-type requirement — a member whose
// type is inadmissible ... does not disqualify the record when marked
// `equality skip`". Pre-fix the importer could not see the clause, re-derived
// the membership computation without the rescue, and REJECTED the `=` at
// compile time (-1) on a program the declaring module compiles and runs.
// ===========================================================================
BEZEL_TEST(xmod_equality_skip_clause_4643,
           imported_record_rescued_by_skip_is_auto_hashable_9_2) {
    int r = compileImportAndRun4643(
        kLib4643,
        "module App4643T3; import Lib4643; "
        "public class MainT3 "
        "  public static method Main(): int "
        "    using var b1 := new Box4643(); "
        "    using var b2 := new Box4643(); "
        "    var h1 := new Holder4643(3, b1); "
        "    var h2 := new Holder4643(3, b2); "
        "    if h1 = h2 then return 47; end; "
        "    return 0; "
        "  end Main; "
        "end MainT3;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 47);
}

// ===========================================================================
// T4 — THE CONTROL, in the opposite direction. `Plain4643` is T1's record
// with the clause REMOVED, so its second component DOES participate and the
// two values must compare UNEQUAL across the same boundary. This row passes
// both pre- and post-fix by construction; it exists so that the other way to
// turn T1 green — treating every imported component as skipped, which the
// symbol bit's `false` default would do if it were inverted — cannot be
// mistaken for the fix.
// ===========================================================================
BEZEL_TEST(xmod_equality_skip_clause_4643,
           imported_positional_unskipped_component_still_compared_9_2) {
    int r = compileImportAndRun4643(
        kLib4643,
        "module App4643T4; import Lib4643; "
        "public class MainT4 "
        "  public static method Main(): int "
        "    var a := new Plain4643(1, 10); "
        "    var b := new Plain4643(1, 20); "
        "    if a = b then return 0; end; "
        "    return 53; "
        "  end Main; "
        "end MainT4;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 53);
}

}  // namespace bezel::module
