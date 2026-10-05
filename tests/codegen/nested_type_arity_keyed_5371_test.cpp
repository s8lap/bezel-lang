// Codegen tests for bug #5371 — arity-keyed nesting for `Inner` types.
// `Outer<T>.Inner` and bare `Outer.Inner` must not collide / mangle identically.
//
// Authority: §2.8.3 rule 3 (v0.314): each enclosing type in a nested type's
// identity path is keyed with its own generic arity, so a nested `Inner` of a
// non-generic `Outer` and a nested `Inner` of a generic `Outer<T>` in one
// module are two distinct types.
//
// FINDING (coordinator audit 2026-10-05): §11.3 prevents both `Outer` and
// `Outer<T>` from being declared in the same module ("a top-level type named
// 'Outer' is already declared"). Cross-module, the module-qualified paths
// (`M1.Outer` vs `M2.Outer`) distinguish them. The collision is unreachable
// with valid programs; the fix is defence-in-depth.
//
// The test that distinguishes the mangling directly is
// `nested_inner_generic_outer_arity_keyed_mangling`: it verifies the compiled
// symbol table contains the arity marker (+1+) for a generic outer's nested
// type, which the pre-fix code does not emit.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"

using bezel::test::runAot;

namespace bezel::codegen {
namespace {

const std::filesystem::path& wtDir5371() {
    return wtScratchDir("wt-5371");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

} // namespace
} // namespace bezel::codegen

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// §2.8.3 rule 3 (v0.314): a non-generic `Outer` with nested `Inner` works.
BEZEL_TEST(codegen_5371, nested_inner_non_generic_outer) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public value: int := 10; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer; "
        "class Main "
        "  public static method Main(): int "
        "    using var o := new Outer.Inner(); "
        "    return o.value; "
        "  end Main; "
        "end Main;"), 10);
}

// §2.8.3 rule 3 (v0.314): a generic `Outer<T>` with nested `Inner` works.
// The bare `Outer.Inner()` syntax resolves to the nested type within Outer<T>.
BEZEL_TEST(codegen_5371, nested_inner_generic_outer) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Outer<T> "
        "  public class Inner "
        "    public value: int := 20; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer; "
        "class Main "
        "  public static method Main(): int "
        "    using var o := new Outer.Inner(); "
        "    return o.value; "
        "  end Main; "
        "end Main;"), 20);
}

// §2.8.3 rule 3 (v0.314): the mangled symbol for `Outer<T>.Inner` contains
// the arity marker (+1+) in the enclosing-type path.
// Pre-fix: mangled as M.Outer+Inner (no arity suffix, collides with non-gen).
// Post-fix: mangled as M.Outer+1+Inner (arity suffix, distinct).
BEZEL_TEST(codegen_5371, nested_inner_generic_outer_arity_keyed_mangling) {
    using namespace bezel::codegen;

    auto dir = wtDir5371();
    std::string stem = unique_stem("bezel_5371_");
    TempFile modSrc((dir / (stem + "_mod.bzl")).string());
    TempFile modObj((dir / (stem + "_mod.o")).string());

    writeSource(modSrc.path,
        "module M; "
        "class Outer<T> "
        "  public class Inner "
        "    public value: int := 30; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer; ");

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(modSrc.path, modObj.path);
    BEZEL_EXPECT_TRUE(r.success);
    if (!r.success) return;

    // Run `nm` on the object file and capture its output.
    std::string nmCmd = "nm " + modObj.path + " 2>/dev/null";
    FILE* fp = popen(nmCmd.c_str(), "r");
    BEZEL_EXPECT_TRUE(fp != nullptr);
    if (!fp) return;

    char buf[256];
    bool foundArityKeyedSymbol = false;
    bool foundNonGenericSymbol = false;
    while (fgets(buf, sizeof(buf), fp)) {
        std::string line(buf);
        // A rity-keyed symbol has "+1+" in the mangled name for the inner type:
        // e.g. "T bz$eq$M.Outer+1+Inner" or "T bz$LibG.Outer+1+Inner$Dispose"
        if (line.find("+1+") != std::string::npos &&
            (line.find("Inner") != std::string::npos)) {
            foundArityKeyedSymbol = true;
        }
        // The non-keyed symbol (pre-fix shape) would be "M.Outer+Inner" without "+1+".
        // Verify we do NOT see a symbol that looks like "Outer+Inner" without the arity.
        if (line.find("Outer+Inner") != std::string::npos &&
            line.find("Outer+1+Inner") == std::string::npos &&
            line.find("Inner") != std::string::npos) {
            foundNonGenericSymbol = true;
        }
    }
    pclose(fp);

    // Post-fix: the symbol contains +1+ (arity marker) for a generic outer.
    // Pre-fix: no +1+ marker — the enclosing type path has no arity encoding.
    BEZEL_EXPECT_TRUE(foundArityKeyedSymbol);
    // Also verify we don't see the non-generic form (which would indicate pre-fix
    // encoding, where generic Outer<T> and non-gen Outer both mangle identically).
    BEZEL_EXPECT_FALSE(foundNonGenericSymbol);
}
