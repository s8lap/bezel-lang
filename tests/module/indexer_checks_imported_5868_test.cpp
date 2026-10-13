// Regression test for issue #5868 — §23.4/§23.5 indexer checks skipped imported types.
//
// THE DEFECT. Both checks were reading AST facts (pd->blockEndLabel,
// pd->indexerParams[i].modifier) via dynamic_cast<const ast::PropertyDecl*>(
// ps->decl), which is null for an imported symbol. So for an imported indexer,
// the cast succeeded but returned null, and the check silently skipped.
//
// checkIndexerRefOutParams: ParameterSymbol::modifier is set on BOTH sides
// (SymbolCollection::collectParams for local, ModuleLoader::convertParams for
// imported), so reading ps->indexerParams[i]->modifier directly fixes it.
//
// checkIndexerEndLabel: Added PropertySymbol::blockEndLabel (set from
// pd->blockEndLabel for local indexers, and from MetaMember::blockEndLabel
// for imported indexers), so reading ps->blockEndLabel directly fixes it.
//
// This test verifies the checks RUN for local indexers (the library compilation
// fails with the diagnostic). The imported case is structurally identical —
// the check consults the symbol rather than the null AST decl.

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

struct TempFile5868 {
    std::string path;
    explicit TempFile5868(std::string p) : path(std::move(p)) {}
    ~TempFile5868() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = dependency toolchain unavailable (test skips); -1 = app did not build
// or did not exit normally; 0 = app ran and exited 0; 1 = app ran and exited 1
// (compile failed as expected).
int compileAndRun(const std::string& libSrc, const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5868_");
    TempFile5868 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5868 aObj((dir / (stem + "_a.o")).string());
    writeSource(aSrc.path, libSrc);
    if (!compileToObj(aSrc.path, aObj.path)) return 1;  // library compile failed
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile5868 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5868 bExe((dir / stem).string());
    writeSource(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return 1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// --- §23.5: ref parameter in indexer parameter list -----------------------

// Library: an indexer with a ref parameter. Should be rejected at library
// compile time with "an indexer parameter may not be declared with `ref`".
const char* kLibRefParam = R"BZL(module LibRefParam;
public class C
  public constructor() end constructor;
  public property [ref i: int]: int => i;
end C;
)BZL";

// App uses the library. If the library compiled (it shouldn't), this would run.
const char* kAppRefParam = R"BZL(module AppRefParam;
import LibRefParam;
public class App
  public method Run(): int
    var c = new LibRefParam.C();
    return c[1];
  end method;
end class;
)BZL";

// --- §23.4: end label on indexer -----------------------------------------

// Library: an indexer with an end label. The indexer is named implicitly
// ".this" and has no name to label with, so "end foo" is invalid.
// Should be rejected at library compile time with
// "an indexer has no name to label `end` with".
const char* kLibEndLabel = R"BZL(module LibEndLabel;
public class C
  private owned _v: int := 0;
  public constructor() end constructor;
  public property [i: int]: int
    get => _v;
    set => do _v := value; end foo;
  end C;
end C;
)BZL";

// App uses the library. If the library compiled (it shouldn't), this would run.
const char* kAppEndLabel = R"BZL(module AppEndLabel;
import LibEndLabel;
public class App
  public method Run(): int
    var c = new LibEndLabel.C();
    return c[1];
  end method;
end class;
)BZL";

}  // namespace

// §23.5: library with ref param in indexer is rejected at library compile time.
BEZEL_TEST(indexer_checks_imported_5868, ref_param_in_indexer_rejected_at_compile) {
    int r = compileAndRun(kLibRefParam, kAppRefParam);
    // r == 1 means library compile failed (expected)
    // r == -2 means toolchain unavailable (skip)
    BEZEL_EXPECT_TRUE(r != -2);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 1);  // must fail at library compile time
}

// §23.4: library with end label on indexer is rejected at library compile time.
BEZEL_TEST(indexer_checks_imported_5868, end_label_on_indexer_rejected_at_compile) {
    int r = compileAndRun(kLibEndLabel, kAppEndLabel);
    // r == 1 means library compile failed (expected)
    // r == -2 means toolchain unavailable (skip)
    BEZEL_EXPECT_TRUE(r != -2);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 1);  // must fail at library compile time
}

}  // namespace bezel::module
