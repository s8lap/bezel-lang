// Regression test for #3468 divergence 2 (spec §29.7 / §29.2 / §2.5, v0.286):
// the CROSS-MODULE face of the drifted frame-slot mirror.
//
// `forInBuiltinEnumeratorNeedsFrameSlot` used to re-derive
// `lowerIEnumerableBuiltinFor`'s "is this for-in source vtable-backed?"
// decision. One of its four divergences was that its `checkPureIEnumerable`
// required `rt->astNode` — the LOCALLY-WRITTEN type annotation — where the
// lowering (since #3187) also reads `rt->importedName`. A TypeRef
// reconstructed from cross-module metadata has NO AST node, so an
// `IEnumerable<T>`-typed local holding an IMPORTED iterator answered `false`
// in the predicate and `vtable-backed` in the lowering: no §29.7 frame field
// was reserved, the enumerator round-tripped only through the alloca spill
// area the state machine's `Dispose` cannot read, and abandoning the enclosing
// iterator mid-loop leaked the enumerator clone every round.
//
// This is the one divergence that CANNOT be reached from a single module (a
// locally-spelled annotation always has an AST node), which is why it lives in
// the module tier rather than beside its four siblings in
// tests/codegen/forin_builtin_enumerator_frame_slot_3468_test.cpp.
//
// ORACLE. The leaked object is a compiler-synthesised enumerator frame, so no
// user destructor can observe it: the program is built under generated-code
// AddressSanitizer and run under LeakSanitizer, and the abandonment is
// amplified to 200 rounds so the leak cannot hide as a single
// still-stack-reachable allocation. `Main` returns 7 only when the folded value
// is also right, so a clean-but-wrong program is not accepted: a leak exits
// with LSan's own status, a wrong answer exits 9, and only the correct,
// leak-free program exits 7.
//
// Spec authority: §29.7 *Disposal of partially-consumed iterators*, §29.2
// *Desugaring of `for`*, §2.5 imported type names; stdlib §4.12–§4.13 (v0.286).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3468 {
    std::string path;
    explicit TempFile3468(std::string p) : path(std::move(p)) {}
    ~TempFile3468() { std::remove(path.c_str()); }
};

void writeSource3468(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile the library module to an object, then compile `mainSrc` against it
// under generated-code AddressSanitizer (the recipe runAotChecked uses), link,
// run, and return the exit status. -1 / -2 on a build failure.
int buildImportAndRunChecked3468(const std::string& libSrc,
                                 const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz3468_");
    TempFile3468 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile3468 libObjF((dir / (id + "_lib.o")).string());
    TempFile3468 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile3468 mainExeF((dir / id).string());
    writeSource3468(libSrcF.path, libSrc);
    writeSource3468(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps, opts).success)
        return -1;

    int status = std::system(mainExeF.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The library: a yield iterator returning `IEnumerable<Item3468>`. Its return
// TypeRef reaches the app module through metadata, with no AST node.
const char* kLib3468 =
    "module VLib3468; "
    "public class Item3468 "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item3468; "
    "public class Fac3468 "
    "  public static method Two(): IEnumerable<Item3468> "
    "    yield return new Item3468(3); "
    "    yield return new Item3468(4); "
    "  end Two; "
    "end Fac3468; ";

// The app: `var seq := Fac3468.Two();` gives the local an IMPORTED TypeRef
// (inferred from the cross-module call, so `astNode` is null and the head lives
// in `importedName`). `Walk` iterates it and yields from inside that loop; the
// consumer abandons `Walk` at the first element, suspending it INSIDE the
// for-in — the §29.7 shape whose enumerator the frame field exists to reach.
std::string app3468(bool abandon) {
    return std::string(
        "module App3468; "
        "import VLib3468; "
        "class Src "
        "  public method Walk(): IEnumerable<int> "
        "    var seq := Fac3468.Two(); "
        "    for var x in seq do "
        "      yield return x.V; "
        "    end for; "
        "  end Walk; "
        "end Src; "
        "class Main "
        "  public static method Run(): int "
        "    using var s := new Src(); "
        "    var t := 0; "
        "    for var v in s.Walk() do "
        "      t := t * 10 + v; ") +
        (abandon ? "      break; " : "") +
        "    end for; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      t := t + Run(); "
        "      i := i + 1; "
        "    end while; " +
        (abandon ? "    if t = 600 then return 7; end if; "
                 : "    if t = 6800 then return 7; end if; ") +
        "    return 9; "
        "  end Main; "
        "end Main;";
}

}  // namespace

// §29.7 / §2.5 / #3468 — abandonment inside a for-in over an IMPORTED
// `IEnumerable<T>` local. Pre-fix the frame field was never reserved (the
// predicate's AST-node-only head test) and the enumerator clone leaked one per
// round; post-fix the shared classifier reads `importedName` exactly as the
// lowering does, the field is reserved and wired, and Dispose reclaims it.
BEZEL_TEST(xmod_forin_imported_enumerable_frame_slot_3468,
           imported_enumerable_local_disposed_on_abandonment_29_7) {
    int rc = buildImportAndRunChecked3468(kLib3468, app3468(/*abandon=*/true));
    BEZEL_EXPECT_EQ(rc, 7);
}

// Control: the same program drained to exhaustion. The loop's own scope-exit
// disposal is then the SOLE disposal of the enumerator — giving the for-in a
// frame field must not make Dispose double-free it (ASan reports that exactly
// as loudly as the leak).
BEZEL_TEST(xmod_forin_imported_enumerable_frame_slot_3468,
           imported_enumerable_local_full_drain_disposes_once_29_7) {
    int rc = buildImportAndRunChecked3468(kLib3468, app3468(/*abandon=*/false));
    BEZEL_EXPECT_EQ(rc, 7);
}

}  // namespace bezel::module
