// Spec-derived regression test for issue #5641 (fix-forward) — §15.1 / §3.6
// rule 2: a nullable use of a type parameter substituted by an inline array
// must be rejected at the INSTANTIATION SITE, including across a module
// boundary.
//
// ── THE DEFECT (MEASURED, two-module, on the staging tip 987a1b94) ──────────
//
// The #5641 check in `ExprTC` walked the instantiating type's fields via
// `dynamic_cast<const ast::TypeDecl*>(t->decl)`. `ModuleLoader` never assigns
// `Symbol::decl` for an IMPORTED type (#3141) — there is no `ast::TypeDecl`
// for a dependency — so the guard's false branch was taken for every imported
// type and the WHOLE check was skipped. A generic imported from another module
// and instantiated with an inline array at a nullable field position therefore
// compiled CLEAN, where §15.1 requires a reject: a silent acceptance, the
// defect class `scripts/check_imported_symbol_reads.py` exists to catch (the
// coordinator's own pick of 66cb9d38 tripped it).
//
//   lib.bzl                        app.bzl
//   module LibA;                   module AppB; import LibA;
//   public class Box<T>            public class Program
//     public _v: T? := null;         public static method Main()
//     public constructor()            using var b := new LibA.Box<[3]int>();
//       end constructor;              end Main;
//   end Box;                       end Program;
//
//   $ bezelc -c -o libA.o libA.bzl
//   $ bezelc --import libA.o -o appB appB.bzl ; echo $?
//   0        <-- PRE-FIX: ACCEPTED, silently. Nothing warns.
//   1        <-- POST-FIX: `nullable inline-array` diagnostic.
//
// A SAME-MODULE test cannot detect this class at all — that is exactly why the
// original commit's conformance row (tests/conformance/rules/cases/s15_1_0007,
// single-module) stayed green through the defect. Every row below is
// two-module, driving the real `Driver::compileObject` + `Driver::compileFile`
// with a real `Dependency`, which is the AOT path.
//
// ── WHY THE REWRITE WAS AVAILABLE (no SEAM-DEBT marker needed) ─────────────
//
// Both facts this check needs are already carried on the symbol side, and
// `scripts/check_imported_symbol_reads.py` names them:
//   * the FIELD LIST rides `TypeSymbol::members` — populated by
//     `SymbolCollection::collectMember` locally and by `ModuleLoader::
//     convertMember` for an import;
//   * the FIELD'S DECLARED TYPE rides `sema::storageComponentType(fs, ctx)` —
//     THE both-sources accessor for exactly this question (`posParam->type`,
//     then the body `FieldDecl` type via `ctx.resolvedType`, then
//     `ctx.importedMemberType(m)`, the loader's `fieldTypes_` side table filled
//     from `MetaMember::type`). It is the accessor #3145 built for the §9.7
//     borrow-bearing-record family, whose audit row is the same
//     silent-acceptance shape.
// The name test uses `TypeRef::headName()`: local TypeRefs carry the name on
// `astNode`, imported ones have `astNode == nullptr` and carry it in
// `importedName`. `headName()` is the one read both spellings agree on.
//
// ── TEST MATRIX ─────────────────────────────────────────────────────────────
//
//   T1  REJECT — imported `Box<[3]int>`, field `public _v: T?`.
//              THE FAIL-WITHOUT HINGE. Pre-fix rc=0 (accepted silently).
//              Falsified by construction: making the field non-nullable
//              (`public _v: T := default(T)`) turns this green — T3.
//   T2  REJECT — LOCAL control, the byte-identical single-module program.
//              Proves the rewrite did not weaken the same-module path, which
//              is the path conformance row s15_1_0007 already covers.
//   T3  ACCEPT — imported `Box<[3]int>`, field NON-nullable `T`. The inline
//              array arg is fine; only a NULLABLE use of `T` is the error.
//   T4  ACCEPT — imported `Box<string>`, field `public _v: T?`. The nullable
//              field is fine; only an INLINE-ARRAY arg is the error.
//
// T3 and T4 are what stop the row from being "rejects anything with a
// nullable field or an inline array" — each removes exactly one of the two
// conditions the rule requires together.

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
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

struct TempFile5641 {
    std::string path;
    explicit TempFile5641(std::string p) : path(std::move(p)) {}
    ~TempFile5641() { std::remove(path.c_str()); }
};

void writeSource5641(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to a library object. Returns true on success.
bool compileLibToObj5641(const std::string& libSrc, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(libSrc, obj).success;
}

// Compile a SINGLE-MODULE program. Returns true on success.
bool compileAlone5641(const std::string& src, const std::string& exe) {
    bezel::Driver driver;
    return driver.compileFile(src, exe).success;
}

// One importing compilation, with its stderr captured. Returns the compile's
// success verdict; `err` receives the captured stderr.
//
// The capture exists so a row can assert the SPECIFIC diagnostic rather than
// merely "did not compile" — `Driver::compileFile` prints diagnostics to stderr
// and exposes only `success`/`errorCount`. Without it a row would be satisfied
// by any compile failure, including a PARSE error, which is exactly the
// vacuous oracle the 5868 fix-forward had to correct.
bool importCompile5641(const std::string& bSrc, const std::string& bExe,
                       const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz5641_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    bool ok = driver.compileFile(bSrc, bExe, deps).success;
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
    return ok;
}

// The verdict for one lib -> app build. `libCompiled` distinguishes "the
// reject came from the check" from "the library never built" — a library that
// does not compile is a VACUOUS green for any reject row.
struct ImportVerdict5641 {
    bool libCompiled = false;
    bool appCompiled = false;  // true = the importing compile SUCCEEDED
    std::string err;
};

ImportVerdict5641 importCompile5641(const std::string& libSrc,
                                    const std::string& bSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5641_");
    TempFile5641 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5641 aObj((dir / (stem + "_a.o")).string());
    TempFile5641 bFile((dir / (stem + "_b.bzl")).string());
    TempFile5641 bExe((dir / stem).string());
    ImportVerdict5641 v;
    writeSource5641(aSrc.path, libSrc);
    writeSource5641(bFile.path, bSrc);
    v.libCompiled = compileLibToObj5641(aSrc.path, aObj.path) &&
                    std::ifstream(aObj.path).good();
    if (!v.libCompiled) return v;
    v.appCompiled = importCompile5641(bFile.path, bExe.path, aObj.path, v.err);
    return v;
}

// --- the library: a nullable type-parameter field -----------------------
//
// `public` (not `private`) because §13.1 drops a `private` member from the
// exported metadata entirely (ModuleMetadataBuilder's `isExportedMember`), so
// a private field genuinely does not exist downstream and cannot be a test of
// the seam. `public` puts the field — and its DECLARED TYPE — across the
// boundary, which is the fact the check needs to read.
const char* kLibNullable = R"BZL(module LibA;
public class Box<T>
  public _v: T? := null;
  public constructor() end constructor;
end Box;
)BZL";

const char* kAppInlineArray = R"BZL(module AppB;
import LibA;
public class Program
  public static method Main()
    using var b := new LibA.Box<[3]int>();
  end Main;
end Program;
)BZL";

// Same library with the field NON-nullable: the inline-array ARG is present
// but no nullable use of `T` exists, so §15.1 admits it.
const char* kLibNonNullable = R"BZL(module LibCtl;
public class Box<T>
  public _v: T := default(T);
  public constructor() end constructor;
end Box;
)BZL";

const char* kAppInlineArrayCtl = R"BZL(module AppCtl;
import LibCtl;
public class Program
  public static method Main()
    using var b := new LibCtl.Box<[3]int>();
  end Main;
end Program;
)BZL";

// The nullable field with a NON-inline-array argument: `string` admits a
// nullable form, so the instantiation is fine.
const char* kAppStringArg = R"BZL(module AppCtl2;
import LibA;
public class Program
  public static method Main()
    using var b := new LibA.Box<string>();
  end Main;
end Program;
)BZL";

// The LOCAL control: byte-identical to kLibNullable + kAppInlineArray minus the
// module boundary. §15.1 rejects it on the single-module path too, and it must
// keep doing so — the rewrite routes the local side through the same accessor.
const char* kLocalNullableInlineArray = R"BZL(module Conf;
public class Box<T>
  private _v: T? := null;
  public constructor() end constructor;
end Box;
public class Program
  public static method Main()
    using var b := new Box<[3]int>();
  end Main;
end Program;
)BZL";

}  // namespace

// T1: THE HINGE. Imported generic, nullable `T` field, inline-array argument.
// Pre-fix this compiled with exit 0 and no diagnostic at all.
BEZEL_TEST(imported_nullable_inline_array_param_5641,
           imported_nullable_param_field_rejects_inline_array_arg) {
    ImportVerdict5641 v = importCompile5641(kLibNullable, kAppInlineArray);
    // The library itself must be valid, or the row is vacuous: a library that
    // does not build satisfies "the app did not build" for the wrong reason.
    BEZEL_EXPECT_TRUE(v.libCompiled);
    BEZEL_EXPECT_FALSE(v.appCompiled);
    // ... and the reject must be THIS rule, not any other compile failure.
    BEZEL_EXPECT_TRUE(v.err.find("nullable inline-array") != std::string::npos);
}

// T2: the LOCAL control. The same rule, same program, no module boundary —
// the path conformance row s15_1_0007 covers. It must still reject, so the
// rewrite did not trade an imported reject for a lost local one.
BEZEL_TEST(imported_nullable_inline_array_param_5641,
           local_nullable_param_field_still_rejects) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5641L_");
    TempFile5641 src((dir / (stem + ".bzl")).string());
    TempFile5641 exe((dir / stem).string());
    writeSource5641(src.path, kLocalNullableInlineArray);
    BEZEL_EXPECT_FALSE(compileAlone5641(src.path, exe.path));
}

// T3: ACCEPT control — inline-array argument, NON-nullable field. Removing the
// `?` removes the error, which is what makes T1 a test of THIS rule rather
// than a test of "imported generics are rejected".
BEZEL_TEST(imported_nullable_inline_array_param_5641,
           imported_non_nullable_param_field_accepts_inline_array_arg) {
    ImportVerdict5641 v =
        importCompile5641(kLibNonNullable, kAppInlineArrayCtl);
    BEZEL_EXPECT_TRUE(v.libCompiled);
    BEZEL_EXPECT_TRUE(v.appCompiled);
}

// T4: ACCEPT control — nullable field, NON-inline-array argument. `string`
// admits a nullable form, so `Box<string>` is well-formed.
BEZEL_TEST(imported_nullable_inline_array_param_5641,
           imported_nullable_param_field_accepts_non_inline_array_arg) {
    ImportVerdict5641 v = importCompile5641(kLibNullable, kAppStringArg);
    BEZEL_EXPECT_TRUE(v.libCompiled);
    BEZEL_EXPECT_TRUE(v.appCompiled);
}

}  // namespace bezel::module