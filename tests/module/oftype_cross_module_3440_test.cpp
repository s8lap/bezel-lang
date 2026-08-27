// narrow issue 3440 — the CROSS-MODULE accept-controls for the `OfType<U>()`
// soundness gate (stdlib §5 *Filtering*; spec §2.5, §8.1, §12; v0.283).
//
// WHY THESE LIVE IN THEIR OWN FILE. The gate in
// `ExprTC::ofTypeIdentityRelabelIsSound` decides two of its three arms on
// `TypeSymbol*` IDENTITY — `functionSlotTypeRefEqual` compares `declaration`
// pointers, and `ExprTC::isSubtype` walks `baseTypes` by pointer — and its
// third arm reads `declaration->moduleName`. Every one of those is a property
// of how the symbol was CREATED, and an imported type's symbol is created by a
// different code path (`ModuleLoader`, from `.bezel.meta`) than a local one
// (`SymbolCollection`). A same-module test cannot see a divergence between them.
//
// Two concrete hazards these pin, neither hypothetical:
//
//   (1) If an imported class resolved to a DIFFERENT `TypeSymbol*` at the
//       receiver's element position than at the `OfType<U>` type-argument
//       position, the identity arm would fail and `xs.OfType<Dog>()` — the same
//       type on both sides — would be REJECTED across a module boundary while
//       compiling fine within one.
//   (2) The §8.1 arm requires `declaration->moduleName` to be NON-EMPTY, which
//       is what excludes compiler-intrinsic `builtinClassSym`s (`Range<T>`,
//       `Ptr<T>` — see the sema test file). If `ModuleLoader` left `moduleName`
//       empty on a reconstructed symbol, every imported class would be
//       misclassified as an intrinsic and `OfType<Object>()` would break
//       cross-module only.
//
// Both are ACCEPT-controls: they assert the values the programs produced BEFORE
// the gate existed, so they fail if the gate over-rejects and also if it
// silently changes what the accepted lowering returns. Measured identical on
// 1e3861a (pre-gate) and on the gated compiler.
//
// The single-module controls, the reject rows, and the full rationale are in
// `tests/sema/oftype_unsound_relabel_3440_test.cpp`.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

const std::filesystem::path& wtDir3440() { return wtScratchDir("wt-3440"); }

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile main importing it -> executable, run.
// Returns the exit code, or -1 on any compile/link/spawn failure. A REJECTION
// by the gate therefore shows up as -1, which no expected value below equals.
int compileAndRunCrossModule(const std::string& libSrc,
                             const std::string& mainSrc) {
    auto dir = wtDir3440();
    std::string stem = unique_stem("bezel_oftype3440");
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / (stem + "_main")).string());

    writeSource(lSrc.path, libSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    if (!rl.success) return -1;

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath   = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    bezel::CompileResult rm = driver.compileFile(mSrc.path, mExe.path, deps);
    if (!rm.success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

const char* kLib = R"BZL(
module LibA;
public class Animal
  public Tag: int := 1;
  public virtual method Who(): int return 0; end Who;
end Animal;
public class Dog: Animal
  public DogOnly: int := 77;
  public override method Who(): int return 1; end Who;
end Dog;
public class Factory
  public static method MakeDogs(): owned OwningArray<Dog>
    var xs := new OwningArray<Dog>();
    xs.Add(new Dog());
    return xs;
  end MakeDogs;
end Factory;
)BZL";

}  // namespace

// An IMPORTED class used as BOTH the sequence element type and as `U`: once at
// `U == T` (the identity arm) and once at `U` = its imported base (the §12
// upcast arm). Both arms compare `TypeSymbol*` pointers, so this is the row
// that would catch a per-resolution-site symbol duplication.
// 77 (DogOnly, identity arm) + 1 (Tag) + 1 (virtual Who() reaching Dog's
// override through the Animal view) = 79.
BEZEL_TEST(module_oftype_3440, accept_imported_class_as_element_and_as_u) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, R"BZL(
module MainB;
import LibA;
class C
  public static method Main(): int
    using var xs := new OwningArray<Dog>();
    xs.Add(new Dog());
    var acc := 0;
    for var d in xs.OfType<Dog>() do
      acc := acc + d.DogOnly;
    end;
    for var a in xs.OfType<Animal>() do
      acc := acc + a.Tag + a.Who();
    end;
    return acc;
  end Main;
end C;
)BZL"), 79);
}

// The receiver's type is RECONSTRUCTED FROM `.bezel.meta` rather than written
// at the call site — it is whatever `Factory.MakeDogs()` was recorded as
// returning. This is the shape where a `declaration` can legitimately come back
// null or with a differently-populated `moduleName`, which is exactly what the
// §8.1 arm reads. 77 (DogOnly) + 1 (Tag) = 78.
BEZEL_TEST(module_oftype_3440, accept_receiver_type_from_imported_metadata) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, R"BZL(
module MainD;
import LibA;
class C
  public static method Main(): int
    using var xs := Factory.MakeDogs();
    var acc := 0;
    for var d in xs.OfType<Dog>() do
      acc := acc + d.DogOnly;
    end;
    for var a in xs.OfType<Animal>() do
      acc := acc + a.Tag;
    end;
    return acc;
  end Main;
end C;
)BZL"), 78);
}

// An IMPORTED class at `U = Object` — the §8.1 arm, which admits a source
// element only when `declaration->moduleName` is NON-EMPTY (that is what
// excludes the compiler intrinsics `Range<T>` / `Ptr<T>`). If `ModuleLoader`
// did not populate `moduleName`, an imported class would be misread as an
// intrinsic and this would be rejected while its same-module twin compiles.
BEZEL_TEST(module_oftype_3440, accept_imported_class_to_object_spec_8_1) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, R"BZL(
module MainE;
import LibA;
class C
  public static method Main(): int
    using var xs := new OwningArray<Dog>();
    xs.Add(new Dog());
    var acc := 0;
    for var o in xs.OfType<Object>() do
      acc := acc + 1;
    end;
    return acc;
  end Main;
end C;
)BZL"), 1);
}
