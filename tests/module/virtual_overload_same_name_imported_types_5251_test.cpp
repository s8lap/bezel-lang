// Regression test for #5251 (§12.1 / §2.8.3 rule 4, v0.313): cross-module
// virtual overloads on two same-named types (A.Item / B.Item) collide in the
// vtable slot key.
//
// The bug: `methodSlotKey` spelled an imported Named parameter type using
// `declaration->name` only (e.g., "Item"). Two modules A5 and B5 each
// exporting a class named `Item` produced identical keys "Take#1#Item." for
// their overloads, causing one to overwrite the other's vtable slot.
//
// The fix: spell `moduleName.name.` so A5.Item and B5.Item produce distinct
// keys "Take#1#A5.Item." and "Take#1#B5.Item.".
//
// Test harness: chain-based build following #5214's buildChainAndRun pattern.
// Each module imports all prior ones in the chain; deps accumulate.

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5251 {
    std::string path;
    explicit TempFile5251(std::string p) : path(std::move(p)) {}
    ~TempFile5251() { std::remove(path.c_str()); }
};

void write5251(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string uniqueStem5251() {
    return unique_stem("bz5251_");
}

// Compile every source but the last as a library object (each importing all
// earlier ones), the last as an executable importing them all, and run it.
// Follows the chain pattern from #5214.
// Returns: the program's stdout as an int, or a negative sentinel on failure.
int buildChainAndRun5251(const std::vector<std::string>& srcs) {
    auto dir = fs::temp_directory_path();
    std::string id = uniqueStem5251();
    std::vector<std::unique_ptr<TempFile5251>> keep;
    std::vector<bezel::Dependency> deps;

    for (std::size_t i = 0; i + 1 < srcs.size(); ++i) {
        auto src = std::make_unique<TempFile5251>(
            (dir / (id + "_m" + std::to_string(i) + ".bzl")).string());
        auto obj = std::make_unique<TempFile5251>(
            (dir / (id + "_m" + std::to_string(i) + ".o")).string());
        write5251(src->path, srcs[i]);
        bezel::Driver driver;
        if (!driver.compileObject(src->path, obj->path, deps).success ||
            !std::ifstream(obj->path).good())
            return -999;
        bezel::Dependency d;
        d.metadataPath = obj->path;
        d.objectPath = obj->path;
        deps.push_back(d);
        keep.push_back(std::move(src));
        keep.push_back(std::move(obj));
    }
    TempFile5251 mainSrc((dir / (id + "_main.bzl")).string());
    TempFile5251 mainExe((dir / id).string());
    write5251(mainSrc.path, srcs.back());
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, mainSrc.path, mainExe.path,
                                         deps).success)
        return -999;

    // Capture stdout to a temp file; the program writes its return value there.
    std::string outPath = (dir / (id + "_out.txt")).string();
    TempFile5251 outFile(outPath);
    std::string cmd = mainExe.path + " > " + outPath;
    int status = std::system(cmd.c_str());
    if (status == -1) return -1;
    std::ifstream out(outPath);
    int result = -1;
    out >> result;
    return result;
}

}  // namespace

// ── T1 — headline repro: same-named imported types get distinct slot keys ────────
//
// Module M1A exports Item returning 1. Module M1B exports Item returning 2.
// Module M1C defines Base with two virtual overloads Take(M1A.Item) and
// Take(M1B.Item). App imports M1C and calls Base.Run(new Base()).
//
// Pre-fix: both overloads key "Take#1#Item." → one slot → second call
// dispatches to first body → returns 11*100+11 = 1111.
// Post-fix: distinct keys "Take#1#M1A.Item." and "Take#1#M1B.Item." → two
// slots → correct dispatch → returns 11*100+22 = 1122.
//
// Spec §12.1 / §2.8.3 rule 4, v0.313.
BEZEL_TEST(virtual_overload_same_name_imported_types_5251,
           t1_two_overloads_distinct_keys_returns_1122_12_1_2_8_3) {
    // M1A: exports Item returning 1
    const std::string m1a =
        "module M1A; "
        "public class Item "
        "  public constructor() end constructor; "
        "  public method V(): int return 1; end V; "
        "end Item; ";

    // M1B: exports Item returning 2
    const std::string m1b =
        "module M1B; "
        "public class Item "
        "  public constructor() end constructor; "
        "  public method V(): int return 2; end V; "
        "end Item; ";

    // M1C: imports M1A+M1B, defines Base with two virtual overloads
    const std::string m1c =
        "module M1C; import M1A; import M1B; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public virtual method Take(x: M1A.Item): int return 10 + x.V(); end Take; "  // 11
        "  public virtual method Take(x: M1B.Item): int return 20 + x.V(); end Take; "  // 22
        "  public static method Run(g: Base): int "
        "    using var a := new M1A.Item(); "
        "    using var b := new M1B.Item(); "
        "    return g.Take(a) * 100 + g.Take(b); "
        "  end Run; "
        "end Base; ";

    // App: imports M1C, calls Base.Run(g) on a `using var` Base. The argument
    // is bound (not a `new` temporary) so the row stays leak-free under the
    // gate's LSan leg; the temporary-argument leak is tracked separately.
    const std::string app =
        "module App1; import M1C; "
        "class Main1 "
        "  public static method Main(): int "
        "    using var g := new Base(); "
        "    const r := Base.Run(g); "
        "    Console.WriteLine(r); "
        "    return r; "
        "  end Main; "
        "end Main1; ";

    int code = buildChainAndRun5251({m1a, m1b, m1c, app});
    // Expected: 11*100+22 = 1122
    // Pre-fix: 11*100+11 = 1111
    BEZEL_EXPECT_EQ(code, 1122);
}

// ── T2 — control: single-overload virtual dispatch to Der in separate module ─────
//
// Module M2A exports Item. Module M2C defines Base with one virtual
// Take(M2A.Item). Module M2D defines Der inheriting Base (no override).
// App dispatches through base-typed reference to Der instance.
//
// This is a control: single overload, no collision possible. Verifies
// cross-module virtual dispatch works when the derived class is in a
// separately-compiled module.
//
// Spec §12.1, v0.313.
BEZEL_TEST(virtual_overload_same_name_imported_types_5251,
           t2_single_overload_xmod_der_dispatch_12_1) {
    const std::string m2a =
        "module M2A; "
        "public class Item "
        "  public constructor() end constructor; "
        "  public method V(): int return 1; end V; "
        "end Item; ";

    const std::string m2c =
        "module M2C; import M2A; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public virtual method Take(x: M2A.Item): int return 10 + x.V(); end Take; "
        "end Base; ";

    const std::string m2d =
        "module M2D; import M2C; "
        "public class Der: Base "
        "  public constructor() end constructor; "
        "end Der; ";

    const std::string app =
        "module App2; import M2D; import M2C; import M2A; "
        "class Main2 "
        "  public static method Main(): int "
        "    using var b: Base := new Der(); "
        "    using var it := new M2A.Item(); "
        "    const r := b.Take(it); "
        "    Console.WriteLine(r); "
        "    return r; "
        "  end Main; "
        "end Main2; ";

    int code = buildChainAndRun5251({m2a, m2c, m2d, app});
    // Expected: Base.Take(M2A.Item) = 10 + 1 = 11
    BEZEL_EXPECT_EQ(code, 11);
}

// ── T3 — override of imported virtual reuses base slot (same type) ───────────────
//
// Module M3A exports Item returning 1. Module M3C defines Base with virtual
// Take(M3A.Item). Module M3D overrides Take(M3A.Item) in Der.
//
// The override must produce the same slot key as the base (same type, same
// module), so it correctly reuses the base slot. Dispatch through base-typed
// reference must reach the Der override, returning 111.
//
// Pre-fix: if key mismatch (e.g. using bare name instead of moduleName.name),
// the override takes a different slot than the base. Returns 11.
// Post-fix: override reuses base slot. Returns 111.
//
// Spec §12.1 / §2.8.3 rule 4, v0.313.
BEZEL_TEST(virtual_overload_same_name_imported_types_5251,
           t3_override_reuses_base_slot_12_1_2_8_3) {
    const std::string m3a =
        "module M3A; "
        "public class Item "
        "  public constructor() end constructor; "
        "  public method V(): int return 1; end V; "
        "end Item; ";

    const std::string m3c =
        "module M3C; import M3A; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public virtual method Take(x: M3A.Item): int return 10 + x.V(); end Take; "
        "end Base; ";

    const std::string m3d =
        "module M3D; import M3C; import M3A; "
        "public class Der: Base "
        "  public constructor() end constructor; "
        "  public override method Take(x: M3A.Item): int return 110 + x.V(); end Take; "
        "end Der; ";

    const std::string app =
        "module App3; import M3D; import M3C; import M3A; "
        "class Main3 "
        "  public static method Main(): int "
        "    using var b: Base := new Der(); "
        "    using var it := new M3A.Item(); "
        "    const r := b.Take(it); "
        "    Console.WriteLine(r); "
        "    return r; "
        "  end Main; "
        "end Main3; ";

    int code = buildChainAndRun5251({m3a, m3c, m3d, app});
    // Expected: Der.Take(M3A.Item) = 110 + 1 = 111
    // Pre-fix: base slot called = 10 + 1 = 11
    BEZEL_EXPECT_EQ(code, 111);
}

}  // namespace bezel::module
