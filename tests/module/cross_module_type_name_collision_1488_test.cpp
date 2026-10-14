// Cross-module symbol mangling omits the module name — issue #1488.
//
// §2.2 (Imports / Collisions): "When two imported modules export the same
// public name, the bare name is ambiguous... the fix is qualification (full
// module name or alias)." That rule presumes two same-named public types CAN
// coexist in one program. §2.8.3 rule 3 (Program-wide type identity) makes
// this precise: "A type is identified program-wide by its declaring module
// name, its qualified type name..., and its generic arity. Two modules each
// declaring a public type of the same name declare two unrelated types." And
// rule 4: "`is` tests, `catch` clauses, and case patterns... match identically
// regardless of which artifact produced the value."
//
// Before the fix, codegen's mangling scheme was `bz$<TypeName>$<Member>` with
// NO module component: two modules each declaring `public class Tx` emitted
// IDENTICAL symbols (`bz$Tx$Hello`, `bz$Tx$Dispose`, `bz$Tx$DisposeCore`,
// `bz$Tx$ensureinit`, `bz$Tx$init`, `td$Tx`, `vt$Tx`, ...). Linking both
// objects into one program failed with "multiple definition of `bz$Tx$Hello`"
// — §2.2's whole qualification/alias model was unimplementable in the ABI.
// Worse: even a SINGLE consumer's own compilation unit could not correctly
// reference both types by external declaration (LLVM auto-uniquified the
// second same-named extern as "bz$Tx$Hello.1", a symbol that exists nowhere,
// making cross-module `is`/`catch` on the second type equally unreachable).
//
// The fix (see Emitter::mangledTypeName, LlvmBackend_impl.hpp) qualifies
// every "bz$"/"vt$"/"td$"-family symbol built from a sema::TypeSymbol with its
// DECLARING MODULE's full dotted name (`bz$LibA.Tx$Hello` vs
// `bz$LibD.Tx$Hello`), carried on TypeSymbol::moduleName and populated
// identically at both symbol-creation sites (SymbolCollection for a local
// declaration; ModuleLoader for an imported one) so an exporter's definition
// and every importer's external declaration always agree.
//
// Tests (spec-derived):
//   (a) two modules each exporting a same-named public type link into one
//       executable and BOTH remain independently usable (right symbol binds
//       to right call) — the headline repro from the issue, plus the alias
//       form §2.2 names as the collision's remedy.
//   (b) cross-module `catch` and `is` on each same-named type dispatch/match
//       correctly and do NOT cross-match the other module's same-named type —
//       §2.8.3 rule 4's "one runtime type identity" pinned directly.
//   (c) control: a single-module build with no cross-module collision
//       potential is unaffected by the qualified mangling scheme.

#include "driver/Driver.hpp"
#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile two independent library modules (libASrc, libBSrc) to objects, then
// compile mainSrc importing BOTH as dependencies, link, run, and return the
// exit code (-1 on any compile/link/run failure). Mirrors buildAndRun in
// cross_module_exception_test.cpp, extended to two dependency artifacts —
// exactly the shape #1488's repro needs (one consumer, two same-named-type
// libraries).
int buildAndRunTwoLibs(const std::string& libASrc, const std::string& libBSrc,
                       const std::string& mainSrc, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1488_" + tag + "_" + unique_stem("");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bObj((dir / (stem + "_b.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());

    writeSource(aSrc.path, libASrc);
    writeSource(bSrc.path, libBSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    if (!ra.success) return -1;
    bezel::CompileResult rb = driver.compileObject(bSrc.path, bObj.path);
    if (!rb.success) return -1;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency da;
    da.metadataPath = aObj.path;
    da.objectPath = aObj.path;
    deps.push_back(da);
    bezel::Dependency db;
    db.metadataPath = bObj.path;
    db.objectPath = bObj.path;
    deps.push_back(db);

    bezel::CompileResult rMain = driver.compileFile(mSrc.path, mExe.path, deps);
    if (!rMain.success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Single-module build for the control test.
int buildAndRunSingle(const std::string& src, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1488_single_" + tag + "_" + unique_stem("");
    TempFile mSrc((dir / (stem + ".bzl")).string());
    TempFile mExe((dir / stem).string());
    writeSource(mSrc.path, src);

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(mSrc.path, mExe.path);
    if (!r.success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

// (a) §2.2 / §2.8.3 rule 3: LibA and LibD each declare `public class Tx` with
// a `public static method Hello(): string`. App imports both — LibD via the
// §2.2 alias form (`import LibD as D;`), exercising both remedies §2.2 names
// for the bare-name collision. Before the fix, compiling+linking App failed at
// the native link step ("multiple definition of `bz$Tx$Hello`" etc.) because
// the mangled names carried no module component. After the fix each type's
// symbols are qualified by its declaring module (`bz$LibA.Tx$Hello` vs
// `bz$LibD.Tx$Hello`), so the link succeeds and each call binds to the RIGHT
// module's Hello() — not just "does it link" but "does the right code run":
// return 1 if LibA.Tx.Hello() reads "hello from A", plus 2 if (aliased)
// D.Tx.Hello() reads "hello from D". Expect 3.
BEZEL_TEST(cross_module_type_name_collision_1488,
          same_named_public_types_link_and_both_usable_via_alias_spec_2_2) {
    int code = buildAndRunTwoLibs(
        // liba.bzl
        "module LibA; "
        "public class Tx: Exception "
        "  public static method Hello(): string => \"hello from A\"; "
        "end Tx;",
        // libd.bzl
        "module LibD; "
        "public class Tx: Exception "
        "  public static method Hello(): string => \"hello from D\"; "
        "end Tx;",
        // app.bzl
        "module App; "
        "import LibA; "
        "import LibD as D; "
        "class Program "
        "  public static method Main(): int "
        "    var ok := 0; "
        "    if LibA.Tx.Hello() = \"hello from A\" then ok := ok + 1; end; "
        "    if D.Tx.Hello() = \"hello from D\" then ok := ok + 2; end; "
        "    return ok; "
        "  end Main; "
        "end Program;",
        "alias_usable");
    // Before the fix: -1 (compile/link failure — the multiple-definition
    // error). After the fix: 3 (both calls bind to their own module's Hello).
    BEZEL_EXPECT_EQ(code, 3);
}

// (b) §2.8.3 rule 4: "`is` tests, `catch` clauses... match identically
// regardless of which artifact produced the value" — and, read the other way,
// two DIFFERENT same-named types must never cross-match. LibA.Tx and (aliased)
// D.Tx are both thrown as exceptions and both constructed directly; App's
// catch clauses and `is` tests must dispatch/match each to its OWN type only.
// Before the fix this was unreachable at all (the link failure in test (a)
// blocks any App from existing); this test additionally pins that qualifying
// the mangling did not accidentally make the two types' runtime identity
// (descriptors td$LibA.Tx / td$LibD.Tx, vtables vt$LibA.Tx / vt$LibD.Tx)
// collide or cross-match.
//   +1  catch LibA.Tx catches LibA.Tx's own throw
//   +2  catch D.Tx catches LibD.Tx's own throw (aliased)
//   +4  a LibA.Tx instance IS LibA.Tx
//  +100 (must NOT happen) a LibA.Tx instance IS D.Tx (cross-match bug)
// Expect 7.
BEZEL_TEST(cross_module_type_name_collision_1488,
          is_and_catch_distinguish_same_named_types_spec_2_8_3_rule_4) {
    int code = buildAndRunTwoLibs(
        // liba.bzl
        "module LibA; "
        "public class Tx: Exception "
        "  public Tag: string; "
        "  public constructor(tag: string) Tag := tag; end constructor; "
        "  destructor end destructor; "
        "  public static method Boom(): int throw new Tx(\"A-boom\"); end Boom; "
        "end Tx;",
        // libd.bzl
        "module LibD; "
        "public class Tx: Exception "
        "  public Tag: string; "
        "  public constructor(tag: string) Tag := tag; end constructor; "
        "  destructor end destructor; "
        "  public static method Boom(): int throw new Tx(\"D-boom\"); end Boom; "
        "end Tx;",
        // app.bzl
        "module App; "
        "import LibA; "
        "import LibD as D; "
        "class Program "
        "  public static method Main(): int "
        "    var ok := 0; "
        "    try "
        "      var dummy := LibA.Tx.Boom(); "
        "    catch LibA.Tx => ok := ok + 1; "
        "    end; "
        "    try "
        "      var dummy2 := D.Tx.Boom(); "
        "    catch D.Tx => ok := ok + 2; "
        "    end; "
        "    using var a := new LibA.Tx(\"probe\"); "
        "    if a is D.Tx then ok := ok + 100; end; "
        "    if a is LibA.Tx then ok := ok + 4; end; "
        "    return ok; "
        "  end Main; "
        "end Program;",
        "is_catch_distinguish");
    // Before the fix: -1 (compile/link failure, same root cause as (a)).
    // After the fix: 7 — every check resolves to its own type, none cross-match.
    BEZEL_EXPECT_EQ(code, 7);
}

// (c) Control: a single-module build has no cross-module collision surface at
// all (only one artifact ever defines `Tx`'s symbols), so the module-qualified
// mangling scheme must not change its observable behavior. Confirms the fix
// does not regress the overwhelmingly common single-module case.
BEZEL_TEST(cross_module_type_name_collision_1488,
          single_module_same_pattern_unaffected_control) {
    int code = buildAndRunSingle(
        "module Solo; "
        "class Tx: Exception "
        "  public static method Hello(): string => \"hello from Solo\"; "
        "end Tx; "
        "class Program "
        "  public static method Main(): int "
        "    if Tx.Hello() = \"hello from Solo\" then return 42; end; "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "single_control");
    BEZEL_EXPECT_EQ(code, 42);
}

// (d) §12.7 *Function-typed targets* (v0.338) over §2.8.3 rule 3 (issue
// #5826): a function type's identity includes its slot types' identities, so
// `(LibA.Tx) -> ()` and `(LibD.Tx) -> ()` are different function types even
// though both slots are spelled `Tx`. Tested on values formed in the consumer
// AND on values formed inside each library (the formed-type descriptor is
// shared across artifacts, so a library-formed value must match the
// consumer's spelling of its own type and no other).
// Exit code: 1|2|4|8 for the four matches that must hold; 16..128 for the
// four that must not.
BEZEL_TEST(cross_module_type_name_collision_1488,
          function_type_test_distinguishes_same_named_slot_types_spec_12_7) {
    int code = buildAndRunTwoLibs(
        "module LibA; "
        "public class Tx "
        "  public constructor() end constructor; "
        "end Tx; "
        "public static class HA "
        "  public static method Take(t: Tx) end Take; "
        "  public static method Formed(): Object => HA.Take; "
        "end HA;",
        "module LibD; "
        "public class Tx "
        "  public constructor() end constructor; "
        "end Tx; "
        "public static class HD "
        "  public static method Take(t: Tx) end Take; "
        "  public static method Formed(): Object => HD.Take; "
        "end HD;",
        "module App; "
        "import LibA; "
        "import LibD as D; "
        "class Program "
        "  public static method Main(): int "
        "    var ok := 0; "
        "    var fa: (LibA.Tx) -> () := LibA.HA.Take; "
        "    var fd: (D.Tx) -> () := D.HD.Take; "
        "    var oa: Object := fa; "
        "    var od: Object := fd; "
        "    if oa is (LibA.Tx) -> () then ok := ok + 1; end; "
        "    if od is (D.Tx) -> () then ok := ok + 2; end; "
        "    if oa is (D.Tx) -> () then ok := ok + 16; end; "
        "    if od is (LibA.Tx) -> () then ok := ok + 32; end; "
        "    var la: Object := LibA.HA.Formed(); "
        "    var ld: Object := D.HD.Formed(); "
        "    if la is (LibA.Tx) -> () then ok := ok + 4; end; "
        "    if ld is (D.Tx) -> () then ok := ok + 8; end; "
        "    if la is (D.Tx) -> () then ok := ok + 64; end; "
        "    if ld is (LibA.Tx) -> () then ok := ok + 128; end; "
        "    return ok; "
        "  end Main; "
        "end Program;",
        "fn_type_slot_identity");
    BEZEL_EXPECT_EQ(code, 15);
}

}  // namespace bezel::module
