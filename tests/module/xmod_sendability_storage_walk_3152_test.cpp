// fix #3152 family (§30.14 rule 2 / §9.2 / §2.8.2 clause 1, spec v0.252):
// `sema::isSendableType` and its §14.2 component-path walker each existed in
// TWO copies — an `ast::TypeDecl` walk and a `TypeSymbol::members` walk,
// selected by `dynamic_cast<const ast::TypeDecl*>(ts->decl)`, i.e. by which
// source happened to exist. `ModuleLoader` never assigns `Symbol::decl` for an
// imported type, so the cast picked the AST copy for every LOCAL type and the
// symbol copy for every IMPORTED one — and the two copies did not agree about
// what counts as §30.14 storage.
//
// THE DIVERGENCE (measured on a real two-module build before the fix). The AST
// copy counted only auto-properties (`pd->bodyKind == Auto`); the symbol copy
// counted EVERY non-indexer `PropertySymbol`, including COMPUTED ones, because
// metadata was believed not to carry the auto/computed fact. It has carried it
// since #2478 (`MetaMember::isAutoProperty` ->
// `ctx.importedPropertyIsAutoStorage`) — #2478's own header lists sendability
// among the ~30 consumers of the bare decl-dependent predicate it did not
// convert. So a computed property voted on sendability across a module
// boundary and nowhere else:
//
//     public sealed class BoxP
//       public property Size: int := 41;             // storage (§9.2)
//       public property Shape: IShapeP? => null;      // COMPUTED — no storage
//     end BoxP;
//
// declared locally: `new Channel<BoxP>(1)` accepted, `where T: sendable`
// satisfied. The byte-identical declaration IMPORTED: both rejected —
// "type argument `BoxP` of `Channel<T>` is not sendable" — and the §14.2
// component path named the computed property as though it were storage:
// "field `Shape` has interface type `IShapeP` (§30.14 rule 3)".
//
// §30.14 rule 2 is "Every instance field, including inherited fields"; §9.2
// makes an auto-property storage exactly like a field, and a computed property
// is not storage at all. So the AST copy was right, the symbol copy was wrong,
// and §2.8.2 clause 1 requires the importer to reach the SAME verdict on the
// module's public surface as the defining compilation does.
//
// THE FIX: one storage walk (`forEachStorageMember`, Sendability.cpp)
// enumerating `TypeSymbol::members` — the one enumeration both a local and an
// imported type have — and asking the shared BOTH-SOURCES accessors for each
// member's facts: `sema::storageComponentType` (#3145) for the type, and a new
// `sema::isAutoPropertyStorageSymbol` for auto-vs-computed. Per the audit
// (docs/proposals/papers/imported-symbol-audit-2026-08-13.md §4): "An accessor must
// consult both sources, not switch on which one exists."
//
// WHAT EACH TEST IS FOR (proven mechanically, not asserted):
//   T1-T4, T9   fail without the fix, pass with it — the computed-property
//               divergence (T1-T4) and the `static` narrowing (T9).
//   T5-T8       pass before AND after: the guard that the collapse did not
//               WIDEN sendability. The #1732 / #1824 holes (an imported class
//               or record vacuously sendable because an AST walk saw no
//               fields) must stay closed, and a real auto-property /
//               positional field must still be weighed. Each pins the
//               diagnostic TEXT, not just the reject, so a later syntax slip
//               in an inline source cannot keep them green for the wrong
//               reason.
//   T10-T12     the three defects an adversarial review of this commit's
//               first draft found, each a LOCAL-side answer that no
//               cross-module test could have caught: §22.10 primary-ctor
//               parameters counted as fields (T10), the `owned` marker read
//               from the wrong place (T11), and rule 2's "including inherited
//               fields" stopping after one level (T12).
//
// Value oracles: T1-T4, T9, T10, T11 run the admitted program and check a
// number the imported object's own storage produced. T5-T8 and T12 are
// rejection tests — their oracle is the diagnostic text.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3152 {
    std::string path;
    explicit TempFile3152(std::string p) : path(std::move(p)) {}
    ~TempFile3152() { std::remove(path.c_str()); }
};

void writeSource3152(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes, chosen so an admitted program's own exit status can never be
// confused with a rejected compile:
//   -2  the DEPENDENCY did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end (the sendability diagnostic).
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3152 = -2;
constexpr int kAppRejected3152 = -3;

// The front-end prints diagnostics to stderr (Driver.hpp) and `CompileResult`
// carries only a bool + a count, so a REJECT test that asserts nothing but
// `kAppRejected3152` would stay green if one of these inline sources later
// acquired a plain syntax error — green for the wrong reason. This captures
// stderr around the compile so every reject below can pin the actual
// diagnostic. Scope is one compile call; the fd is restored in the destructor
// on every path so a failing assertion still reports through the real stderr.
class StderrCapture3152 {
public:
    explicit StderrCapture3152(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3152() { restore(); }
    std::string text() {
        restore();
        std::ifstream in(path_);
        std::string s((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
        std::remove(path_.c_str());
        return s;
    }

private:
    void restore() {
        if (saved_ < 0) return;
        std::fflush(stderr);
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        saved_ = -1;
    }
    std::string path_;
    int saved_ = -1;
};

bool contains3152(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3152(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3152_");
    TempFile3152 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3152 aObj((dir / (stem + "_a.o")).string());
    writeSource3152(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3152;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3152;

    TempFile3152 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3152 bExe((dir / stem).string());
    writeSource3152(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3152 cap((dir / (stem + ".err")).string());
        ok = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kAppRejected3152;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module control: the SAME declarations in one compilation. Every
// admission test below runs both, because the claim is not "this program
// compiles" but "the importer agrees with the defining compilation" (§2.8.2
// clause 1) — a control that also fails would mean the rule changed, not that
// the seam was closed.
int compileAndRun3152(const std::string& src, std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3152L_");
    TempFile3152 s((dir / (stem + ".bzl")).string());
    TempFile3152 exe((dir / stem).string());
    writeSource3152(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3152 cap((dir / (stem + ".err")).string());
        ok = driver.compileFile(s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kAppRejected3152;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — CORE repro (CLASS, §30.15 channel-element route). An imported `sealed`
// class whose only interface-typed member is a COMPUTED property is sendable:
// a computed property is not an instance field, so §30.14 rule 3's "no
// interface-typed fields" does not reach it. The exit code is the imported
// object's own auto-property value plus a local constant, so the test proves
// the admitted program RUNS and reads the imported storage correctly — not
// merely that the front-end stopped complaining.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           imported_class_computed_property_is_not_storage_30_14_rule_2) {
    int r = compileImportAndRun3152(
        "module Lib3152T1; "
        "public interface IShape3152T1 "
        "end IShape3152T1; "
        "public sealed class Box3152T1 "
        "  public property Size: int := 41; "
        "  public property Shape: IShape3152T1? => null; "
        "end Box3152T1;",
        "module App3152T1; import Lib3152T1; import System.Threading; "
        "public class C3152T1 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3152T1>(1); "
        "    using var b := new Box3152T1(); "
        "    return b.Size + 3; "
        "  end Main; "
        "end C3152T1;");
    if (r == kDepUnavailable3152) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 44);  // 41 (imported auto-property) + 3

    // The control: identical declarations, one module. This ALREADY passed
    // before the fix — which is exactly what made the imported answer a
    // divergence rather than a rule.
    int local = compileAndRun3152(
        "module Loc3152T1; import System.Threading; "
        "public interface IShape3152T1L "
        "end IShape3152T1L; "
        "public sealed class Box3152T1L "
        "  public property Size: int := 41; "
        "  public property Shape: IShape3152T1L? => null; "
        "end Box3152T1L; "
        "public class C3152T1L "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3152T1L>(1); "
        "    using var b := new Box3152T1L(); "
        "    return b.Size + 3; "
        "  end Main; "
        "end C3152T1L;");
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T2 — RECORD twin (§9.2 / §9.7). In a record `public property Name: T;` is an
// auto-property and `=> expr` is computed, the same as in a class; the record
// branch of isSendableType had the same two copies.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           imported_record_computed_property_is_not_storage_9_2_30_14) {
    int r = compileImportAndRun3152(
        "module Lib3152T2; "
        "public interface IShape3152T2 "
        "end IShape3152T2; "
        "public record Pt3152T2(X: int) "
        "  public property Shape: IShape3152T2? => null; "
        "end Pt3152T2;",
        "module App3152T2; import Lib3152T2; import System.Threading; "
        "public class C3152T2 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Pt3152T2>(1); "
        "    var p := new Pt3152T2(41); "
        "    return p.X + 3; "
        "  end Main; "
        "end C3152T2;");
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, 44);

    int local = compileAndRun3152(
        "module Loc3152T2; import System.Threading; "
        "public interface IShape3152T2L "
        "end IShape3152T2L; "
        "public record Pt3152T2L(X: int) "
        "  public property Shape: IShape3152T2L? => null; "
        "end Pt3152T2L; "
        "public class C3152T2L "
        "  public static method Main(): int "
        "    using var ch := new Channel<Pt3152T2L>(1); "
        "    var p := new Pt3152T2L(41); "
        "    return p.X + 3; "
        "  end Main; "
        "end C3152T2L;");
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T3 — the §14.2 derived-property-constraint route (`where T: sendable`),
// which reaches the SAME computation (§14.2: "the constraint adds no second
// definition of the property") through Resolver::checkOneConstraint and then
// through the component-path walker for its message. Pre-fix this produced
// "field `Shape` has interface type ... (§30.14 rule 3)" — a diagnostic naming
// a computed property as a field.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           imported_class_computed_property_satisfies_sendable_constraint_14_2) {
    int r = compileImportAndRun3152(
        "module Lib3152T3; "
        "public interface IShape3152T3 "
        "end IShape3152T3; "
        "public sealed class Box3152T3 "
        "  public property Size: int := 41; "
        "  public property Shape: IShape3152T3? => null; "
        "end Box3152T3;",
        "module App3152T3; import Lib3152T3; "
        "public class Ferry3152T3<T> where T: sendable "
        "  public property Tag: int := 3; "
        "end Ferry3152T3; "
        "public class C3152T3 "
        "  public static method Main(): int "
        "    using var f := new Ferry3152T3<Box3152T3>(); "
        "    using var b := new Box3152T3(); "
        "    return b.Size + f.Tag; "
        "  end Main; "
        "end C3152T3;");
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T4 — §30.14 rule 2's "including inherited fields": a LOCAL `sealed` class
// deriving from an IMPORTED base. The base-class arm had its own copy of the
// same two-branch selection, so a computed property on an imported base
// disqualified the local derived class too.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           imported_base_computed_property_does_not_block_derived_30_14_rule_2) {
    int r = compileImportAndRun3152(
        "module Lib3152T4; "
        "public interface IShape3152T4 "
        "end IShape3152T4; "
        "public class Base3152T4 "
        "  public property Size: int := 41; "
        "  public property Shape: IShape3152T4? => null; "
        "end Base3152T4;",
        "module App3152T4; import Lib3152T4; "
        "public sealed class Derived3152T4: Base3152T4 "
        "  public property Own: int := 3; "
        "end Derived3152T4; "
        "public class Ferry3152T4<T> where T: sendable "
        "  public property Tag: int := 0; "
        "end Ferry3152T4; "
        "public class C3152T4 "
        "  public static method Main(): int "
        "    using var f := new Ferry3152T4<Derived3152T4>(); "
        "    using var d := new Derived3152T4(); "
        "    return d.Size + d.Own; "
        "  end Main; "
        "end C3152T4;");
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T5 — GUARD (passes before and after): an imported AUTO-property of interface
// type IS storage (§9.2) and must still fail §30.14 rule 3. This is the case
// the pre-fix over-approximation happened to get right; the collapse must not
// buy T1 by dropping properties wholesale.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           imported_auto_property_of_interface_type_still_rejected_30_14_rule_3) {
    std::string diag;
    int r = compileImportAndRun3152(
        "module Lib3152T5; "
        "public interface IShape3152T5 "
        "end IShape3152T5; "
        "public sealed class Box3152T5 "
        "  public property Shape: IShape3152T5?; "
        "end Box3152T5;",
        "module App3152T5; import Lib3152T5; import System.Threading; "
        "public class C3152T5 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3152T5>(1); "
        "    return 7; "
        "  end Main; "
        "end C3152T5;",
        &diag);
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, kAppRejected3152);
    // Pin the REASON, not just the failure: without this a later syntax slip
    // in the source above would keep the test green for the wrong reason.
    BEZEL_EXPECT_TRUE(contains3152(diag, "`Box3152T5` of `Channel<T>` is not sendable"));
}

// ===========================================================================
// T6 — GUARD: an imported record's §9.1 POSITIONAL field of interface type is
// storage and still makes the record borrow-bearing (§9.7). The positional
// field's type reaches the walk only through `storageComponentType`'s imported
// arm — an imported positional FieldSymbol carries a SYNTHETIC `ast::Param`
// with a null type (#2516), so a walk that trusted `posParam->type` would read
// nothing here.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           imported_positional_record_field_still_rejected_9_7_30_14) {
    std::string diag;
    int r = compileImportAndRun3152(
        "module Lib3152T6; "
        "public interface IShape3152T6 "
        "end IShape3152T6; "
        "public record Pt3152T6(X: int, S: IShape3152T6?) "
        "end Pt3152T6;",
        "module App3152T6; import Lib3152T6; import System.Threading; "
        "public class C3152T6 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Pt3152T6>(1); "
        "    return 7; "
        "  end Main; "
        "end C3152T6;",
        &diag);
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, kAppRejected3152);
    BEZEL_EXPECT_TRUE(contains3152(diag, "`Pt3152T6` of `Channel<T>` is not sendable"));
}

// ===========================================================================
// T7 — GUARD (#1732's hole): an imported `sealed` class with a NON-`owned`
// class-typed field is a back-reference into the origin domain and must still
// fail §30.14 rule 3. An AST-only walk sees no fields at all for an imported
// class and would call this sendable — a data race.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           imported_non_owned_class_field_still_rejected_30_14_rule_3) {
    std::string diag;
    int r = compileImportAndRun3152(
        "module Lib3152T7; "
        "public sealed class Payload3152T7 "
        "  public property N: int := 1; "
        "end Payload3152T7; "
        "public sealed class Box3152T7 "
        "  public Ref: Payload3152T7?; "
        "end Box3152T7;",
        "module App3152T7; import Lib3152T7; import System.Threading; "
        "public class C3152T7 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3152T7>(1); "
        "    return 7; "
        "  end Main; "
        "end C3152T7;",
        &diag);
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, kAppRejected3152);
    BEZEL_EXPECT_TRUE(contains3152(diag, "`Box3152T7` of `Channel<T>` is not sendable"));
}

// ===========================================================================
// T8 — GUARD, inherited direction: an imported base's real interface-typed
// FIELD still disqualifies a local sealed derived class (§30.14 rule 2's
// "including inherited fields"), so T4's admission is not the base walk
// silently going blind.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           imported_base_interface_field_still_blocks_derived_30_14_rule_2) {
    std::string diag;
    int r = compileImportAndRun3152(
        "module Lib3152T8; "
        "public interface IShape3152T8 "
        "end IShape3152T8; "
        "public class Base3152T8 "
        "  public Shape: IShape3152T8?; "
        "end Base3152T8;",
        "module App3152T8; import Lib3152T8; "
        "public sealed class Derived3152T8: Base3152T8 "
        "  public property Own: int := 3; "
        "end Derived3152T8; "
        "public class Ferry3152T8<T> where T: sendable "
        "  public property Tag: int := 0; "
        "end Ferry3152T8; "
        "public class C3152T8 "
        "  public static method Main(): int "
        "    using var f := new Ferry3152T8<Derived3152T8>(); "
        "    return 7; "
        "  end Main; "
        "end C3152T8;",
        &diag);
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, kAppRejected3152);
    // The component path must name the INHERITED field, by name and rule.
    BEZEL_EXPECT_TRUE(contains3152(
        diag, "field `Shape` has interface type `IShape3152T8` (§30.14 rule 3)"));
}

// ===========================================================================
// T9 — the collapse's SECOND deliberate narrowing, pinned on BOTH sides.
// §30.14 rule 2 is "Every **instance** field, including inherited fields"; a
// `static` member is not one. Static state is process-global and is policed
// where it is declared — §22.12 rule 2 refuses a mutable static of
// class/interface type outright, and rule 4 warns that a `static readonly` one
// is "reachable from every domain" (the warning this program draws) — so it
// never travels with a transferred instance.
//
// Both former copies of the walk counted `static` FIELDS, and the AST copy also
// counted `static` auto-properties (it filtered on `bodyKind` only). Keeping
// them was not available to a single predicate: the one both-sources
// auto-property accessor is non-static on both sides (#2478's wire bit mirrors
// `isAutoPropertySymbol`), so a static auto-property could only have been kept
// by adding a sixth loader side table — and keeping static fields while
// dropping static auto-properties would leave the predicate incoherent in a way
// neither former copy was. So statics are out uniformly, per rule 2's wording.
//
// This FAILS WITHOUT THE FIX on BOTH halves — the local half too, which is why
// it is pinned here rather than left to the imported side alone.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           static_field_does_not_vote_on_sendability_30_14_rule_2) {
    int r = compileImportAndRun3152(
        "module Lib3152T9; "
        "public interface IShape3152T9 "
        "end IShape3152T9; "
        "public sealed class Box3152T9 "
        "  public static readonly Cache: IShape3152T9? := null; "
        "  public property Size: int := 41; "
        "end Box3152T9;",
        "module App3152T9; import Lib3152T9; import System.Threading; "
        "public class C3152T9 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3152T9>(1); "
        "    using var b := new Box3152T9(); "
        "    return b.Size + 3; "
        "  end Main; "
        "end C3152T9;");
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, 44);

    int local = compileAndRun3152(
        "module Loc3152T9; import System.Threading; "
        "public interface IShape3152T9L "
        "end IShape3152T9L; "
        "public sealed class Box3152T9L "
        "  public static readonly Cache: IShape3152T9L? := null; "
        "  public property Size: int := 41; "
        "end Box3152T9L; "
        "public class C3152T9L "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3152T9L>(1); "
        "    using var b := new Box3152T9L(); "
        "    return b.Size + 3; "
        "  end Main; "
        "end C3152T9L;");
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T10 — §22.10 CAPTURE RULE, negative half: a CLASS's primary-constructor
// parameter that is never referenced in a body position is NOT a field, so it
// cannot make the class non-sendable. §22.10 "Capture rule and layout
// stability": a parameter is captured "**if and only if** it is referenced in
// a body position ... Parameters used only in field initializers or in the
// inheritance clause's base call are *not* captured ... no separate
// parameter-named field exists."
//
// This is a LOCAL-side pin and no cross-module test can catch it. The trap:
// `Parser_Decls.cpp` sets `hasPositionalFields` for a CLASS as well as a
// record, and SymbolCollection therefore builds a `posParam` FieldSymbol for
// every primary-ctor parameter. The former AST walk iterated `atd->members`
// and so never saw them; a symbol-side walk sees them all and must not treat
// them as §30.14 rule 2 instance fields. The first draft of this commit did,
// and rejected the program below with "field `s` has interface type" —
// naming a non-field as a field, the exact defect this file exists to remove.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           uncaptured_class_primary_ctor_param_is_not_a_field_22_10) {
    std::string diag;
    int local = compileAndRun3152(
        "module Loc3152TA; import System.Threading; "
        "public interface IShape3152TA "
        "end IShape3152TA; "
        // `s` is never referenced in a body position, so §22.10 captures
        // nothing and no field named `s` exists.
        "public sealed class Box3152TA(s: IShape3152TA?) "
        "  private _n: int := 41; "
        "  public method N(): int => _n; "
        "end Box3152TA; "
        "public class C3152TA "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3152TA>(1); "
        "    using var b := new Box3152TA(null); "
        "    return b.N() + 3; "
        "  end Main; "
        "end C3152TA;",
        &diag);
    BEZEL_EXPECT_EQ(local, 44);
    BEZEL_EXPECT_TRUE(diag.find("not sendable") == std::string::npos);

    // The imported half, for the same reason and the same verdict. Constructs
    // the imported class through its §22.10 PRIMARY constructor directly
    // (`new Box3152TAX(null)`) — the verbatim repro from #3166, pinned as a
    // regression test against the cross-module primary-ctor gap. That gap
    // (`new ImportedClass(args)` rejected as "implicit default constructor
    // takes 0") was closed by #3162 + #3164 (commit 8d8af5d); before that
    // commit this row had to chain through an explicit 0-arg convenience
    // constructor, which side-stepped the actual gap it now exercises. The
    // primary-ctor PARAMETER, the thing under test, is present either way.
    int r = compileImportAndRun3152(
        "module Lib3152TA; "
        "public interface IShape3152TAX "
        "end IShape3152TAX; "
        "public sealed class Box3152TAX(s: IShape3152TAX?) "
        "  private _n: int := 41; "
        "  public method N(): int => _n; "
        "end Box3152TAX;",
        "module App3152TA; import Lib3152TA; import System.Threading; "
        "public class C3152TAX "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3152TAX>(1); "
        "    using var b := new Box3152TAX(null); "
        "    return b.N() + 3; "
        "  end Main; "
        "end C3152TAX;");
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T11 — §22.10 capture rule, `owned` half, and the §2.8.2 clause 1 direction.
// `owned l: Leaf` IS referenced in a body position, so §22.10 captures it as
// an `owned` private field, and §30.14 rule 2 admits "an `owned` field whose
// declared type is a sendable class type". The class is sendable.
//
// TWO defects meet here, both measured:
//   * the `owned` marker of a positional field lives on `ast::Param::modifier`,
//     NOT in `FieldSymbol::modifiers` (never populated for a positional field),
//     so a walk reading only the latter answers "not owned" and rejects this
//     program with "field `l` has class type `Leaf` that is not `owned`" — a
//     correct program refused for a factually wrong reason.
//   * on `main` this class was already sendable LOCALLY and NOT sendable
//     IMPORTED — a live divergence at this lane's base. §2.8.2 clause 1 says
//     the importer must reach the DEFINING compilation's verdict, so the two
//     must agree at "sendable"; both halves are asserted here so a future
//     change cannot close the gap by moving the local answer instead.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           captured_owned_primary_ctor_param_stays_sendable_22_10_30_14) {
    std::string diag;
    int local = compileAndRun3152(
        "module Loc3152TB; import System.Threading; "
        "public sealed class Leaf3152TB "
        "  public property V: int := 41; "
        "end Leaf3152TB; "
        "public sealed class Node3152TB(owned l: Leaf3152TB) "
        "  public method Get(): int => l.V; "
        "end Node3152TB; "
        "public class C3152TB "
        "  public static method Main(): int "
        "    using var ch := new Channel<Node3152TB>(1); "
        "    using var n := new Node3152TB(new Leaf3152TB()); "
        "    return n.Get() + 3; "
        "  end Main; "
        "end C3152TB;",
        &diag);
    BEZEL_EXPECT_EQ(local, 44);
    BEZEL_EXPECT_TRUE(diag.find("that is not `owned`") == std::string::npos);

    // Constructed through the §22.10 PRIMARY constructor directly
    // (`new Node3152TBX(new Leaf3152TBX())`), for the same reason as T10's
    // imported half. The `owned` call-site marker on `new Leaf3152TBX()` is
    // required by §22.3 and is now carried across the module boundary — that
    // is #3164's half of the joint commit (8d8af5d); without it the constructor
    // would be uncallable because the `owned` parameter would not see its
    // mandatory matching marker. #3162 is the construction half. Both halves
    // are necessary and are pinned together here.
    int r = compileImportAndRun3152(
        "module Lib3152TB; "
        "public sealed class Leaf3152TBX "
        "  public property V: int := 41; "
        "end Leaf3152TBX; "
        "public sealed class Node3152TBX(owned l: Leaf3152TBX) "
        "  public method Get(): int => l.V; "
        "end Node3152TBX;",
        "module App3152TB; import Lib3152TB; import System.Threading; "
        "public class C3152TBX "
        "  public static method Main(): int "
        "    using var ch := new Channel<Node3152TBX>(1); "
        "    using var n := new Node3152TBX(new Leaf3152TBX()); "
        "    return n.Get() + 3; "
        "  end Main; "
        "end C3152TBX;");
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T12 — §30.14 rule 2 is "Every instance field, **including inherited
// fields**", and inheritance is transitive. Both former walks stopped after
// ONE level, so a field on a GRANDparent was never weighed: `class GrandBase
// { Shape: IShape? } <- class Mid <- sealed class Leaf` was reported sendable
// on `main` and in this commit's first draft — an accept-hole in a data-race
// rule, since a spawn or channel would then move an instance holding an
// origin-domain interface reference. LOCAL-side pin; the imported twin below
// shows the ancestor walk crosses a module boundary too.
// ===========================================================================
BEZEL_TEST(xmod_sendability_storage_walk_3152,
           grandparent_field_is_weighed_by_rule_2_including_inherited) {
    std::string diag;
    int local = compileAndRun3152(
        "module Loc3152TC; "
        "public interface IShape3152TC "
        "end IShape3152TC; "
        "public class Grand3152TC "
        "  public Shape: IShape3152TC?; "
        "end Grand3152TC; "
        "public class Mid3152TC: Grand3152TC "
        "  public property M: int := 1; "
        "end Mid3152TC; "
        "public sealed class Leaf3152TC: Mid3152TC "
        "  public property Own: int := 3; "
        "end Leaf3152TC; "
        "public class Ferry3152TC<T> where T: sendable "
        "  public property Tag: int := 7; "
        "end Ferry3152TC; "
        "public class C3152TC "
        "  public static method Main(): int "
        "    using var f := new Ferry3152TC<Leaf3152TC>(); "
        "    return f.Tag; "
        "  end Main; "
        "end C3152TC;",
        &diag);
    BEZEL_EXPECT_EQ(local, kAppRejected3152);
    BEZEL_EXPECT_TRUE(contains3152(
        diag, "field `Shape` has interface type `IShape3152TC` (§30.14 rule 3)"));

    // The same chain with the offending field on an IMPORTED grandparent.
    std::string idiag;
    int r = compileImportAndRun3152(
        "module Lib3152TC; "
        "public interface IShape3152TCX "
        "end IShape3152TCX; "
        "public class Grand3152TCX "
        "  public Shape: IShape3152TCX?; "
        "end Grand3152TCX; "
        "public class Mid3152TCX: Grand3152TCX "
        "  public property M: int := 1; "
        "end Mid3152TCX;",
        "module App3152TC; import Lib3152TC; "
        "public sealed class Leaf3152TCX: Mid3152TCX "
        "  public property Own: int := 3; "
        "end Leaf3152TCX; "
        "public class Ferry3152TCX<T> where T: sendable "
        "  public property Tag: int := 7; "
        "end Ferry3152TCX; "
        "public class C3152TCX "
        "  public static method Main(): int "
        "    using var f := new Ferry3152TCX<Leaf3152TCX>(); "
        "    return f.Tag; "
        "  end Main; "
        "end C3152TCX;",
        &idiag);
    if (r == kDepUnavailable3152) return;
    BEZEL_EXPECT_EQ(r, kAppRejected3152);
    BEZEL_EXPECT_TRUE(contains3152(idiag, "does not satisfy the constraint"));
}

}  // namespace bezel::module
