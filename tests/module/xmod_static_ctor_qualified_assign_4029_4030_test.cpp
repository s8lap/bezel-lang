// Cross-module — issues #4029/#4030 (§7.9/§2.7, v0.294): a static
// constructor's marking of its OWN type's static-field completion must
// resolve the assignment target by SYMBOL IDENTITY, not by name, so that an
// assignment to an IMPORTED type's same-named static member (reached through
// §2.7's `TypeName.X` spelling) is never mistaken for discharging THIS
// type's own §7.9 obligation.
//
// `DefiniteAssignment.cpp`'s `isOwnStaticMemberTarget` (added by #4029's fix)
// guards its `ps->decl`-dependent auto-property check behind an identity
// check (`ps->parent->decl == staticCtorType_`) that can only be true for a
// LOCAL member — a static constructor can only be authored for a type
// declared in the CURRENT compilation, so `staticCtorType_` is always a
// local `ast::TypeDecl`, and an IMPORTED type's TypeSymbol never has `decl`
// pointing into it. This file is the MEASURED two-module proof #4030's
// review demanded: a static constructor that writes to an IMPORTED type's
// same-named static property must (a) NOT crash, and (b) NOT be credited
// toward the importer's OWN, genuinely-unassigned field of the same name —
// i.e. the import must still be rejected at compile time (§7.9), exactly as
// the identical single-module shape is (see
// tests/sema/static_field_no_ctor_definite_assignment_4015_test.cpp's
// `qualified_assign_to_different_type_same_name_still_rejected_spec_7_9`).

#include "module/ModuleLoader.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a library object, then compile+link `appSrc` against it
// and run the result. Returns the process exit code; -1 if either compile
// step failed (a §2.5-style rejection — the CORRECT outcome for this file's
// row, since the importer's own field is genuinely unassigned) and -2 if the
// program died on a SIGNAL (the wrong-answer shape this fix's own repro
// crashed with before it existed at all).
int buildAndRun(const std::string& stem, const std::string& libSrc,
                const std::string& appSrc) {
    auto dir = std::filesystem::temp_directory_path();
    TempFile a((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile b((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(a.path, libSrc);
    writeSource(b.path, appSrc);

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(a.path, aObj.path);
    if (!ra.success) return -1;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(b.path, bExe.path, deps);
    if (!rb.success) return -1;
    int status = std::system(bExe.path.c_str());
    if (status == -1) return -1;
    if (WIFSIGNALED(status)) return -2;  // died on a signal
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// The importer's OWN static field is left genuinely unassigned (its static
// constructor writes only to the IMPORTED type's same-named field via §2.7's
// `TypeName.X` spelling) — must be rejected at compile time (-1), not
// silently accepted (which would print a real exit code, since the program
// never even reads the field) and not crash (-2, the shape this whole fix
// closes on the single-module axis).
BEZEL_TEST(xmod_static_ctor_qualified_assign_4029_4030,
           qualified_assign_to_imported_type_does_not_credit_importer_7_9) {
    BEZEL_EXPECT_EQ(
        buildAndRun(
            "bezel_4030_reject",
            "module Lib4030A; "
            "public enum NoZero4030A A := 1, B := 2; end NoZero4030A; "
            "public class Base4030A "
            "  public static property Shared: NoZero4030A := NoZero4030A.A; "
            "end Base4030A;",
            "module App4030A; import Lib4030A; "
            "public class Derived4030A "
            "  public static Shared: NoZero4030A; "
            "  static constructor() "
            "    Base4030A.Shared := NoZero4030A.B; "
            "  end constructor; "
            "end Derived4030A; "
            "class C public static method Main(): int return 0; "
            "  end Main; end C;"),
        -1);
}

// Same shape, but the static constructor's qualified assignment targets its
// OWN type (reached through the type's own name, exactly as #4029's
// single-module fix requires) — must compile and run correctly across the
// module boundary, proving the identity check is not so conservative it also
// rejects the legitimate case for an EXPORTING module's own static
// constructor.
BEZEL_TEST(xmod_static_ctor_qualified_assign_4029_4030,
           qualified_assign_to_own_exported_type_accepted_across_boundary) {
    BEZEL_EXPECT_EQ(
        buildAndRun(
            "bezel_4030_accept",
            "module Lib4030B; "
            "public class Base4030B "
            "  public static readonly Shared: string; "
            "  static constructor() "
            "    Base4030B.Shared := \"hello\"; "
            "  end constructor; "
            "end Base4030B;",
            "module App4030B; import Lib4030B; "
            "class C public static method Main(): int "
            "  return Base4030B.Shared.Length; "
            "  end Main; end C;"),
        5);
}

}  // namespace bezel::module
