// fix #3171 (§30.14 rule 2 / §14.2 / §9.7 / §2.8.2 clause 1, spec v0.254):
// the §30.14 class branch in `isSendableType` walked the storage of `decl`
// AND its base chain WITHOUT seeding a `common::RecordSubstEnv`. A generic
// base's field types therefore arrived at the rule in their DECLARATION
// form — bare type parameters (`T`, `U`, …) — and were weighed by the
// conservative "bare type parameter is not sendable" reading.
//
// Measured on the umbrella fix #3152 base (before this commit):
//
//     public class Base<T>        -- T is bare
//       public v: T;
//     end Base;
//     public sealed class Leaf: Base<int>;    -- §30.14 must weigh v as int
//
// `Leaf` was REJECTED at the `Channel<Leaf>` site even though every field
// `Base` contributes at the `Leaf` use site is `int`-typed and therefore
// sendable — a class with NO interface-typed, no class-typed, no
// function-typed field was refused for the type of a bare parameter that
// the substitution at the use site had resolved to a primitive. The
// §30.14/§30.15 channel rule and the §14.2 derived-property constraint
// were both affected — the constraint route also weighed a bare `T`.
//
// The same hole, on the OTHER side of the same generic-base substitution:
// substituting an interface (or other non-sendable type) for `T` made the
// `Leaf` falsely ACCEPTED, because the interface-typed field was read as
// a bare type parameter the conservative reading answers "not sendable"
// (correct direction, wrong reason) — but a substituted primitive `T = int`
// should be ACCEPTED (wrong direction). One defect, two directions.
//
// THE FIX mirrors the record/choice branch pattern (#2211 / #2183 / §35.12):
// the class branch seeds `RecordSubstEnv` for `decl` from `rt->genericArgs`
// and threads it through both `forEachStorageMember` and
// `forEachInheritedStorageMember`. The inherited walker re-seeds each base's
// env from `ts->baseTypeRefs[i]->genericArgs` (parallel to `baseTypes`,
// populated on both sides), with the incoming env so chains like
// `class Leaf<T>: Base<T>` resolve `Base`'s `T` through `Leaf`'s env.
//
// WHAT EACH TEST IS FOR (axis grid: LOCAL+IMPORTED × generic base +
// concrete base):
//
//   T1 — LOCAL generic base, primitive substitution: `sealed class Leaf:
//        Base<int>` must be SENDABLE. Without the fix, REJECTED. (The
//        pre-#3171 verdict was the defect's first direction.)
//   T2 — LOCAL generic base, interface substitution: `sealed class Leaf:
//        Base<IShape>` must be REJECTED (interface field). Without the
//        fix, ACCEPTED (the defect's second direction).
//   T3 — LOCAL concrete base, control: the non-generic base path is
//        unchanged by the fix; the field walk works as before.
//   T4 — IMPORTED generic base, primitive substitution: same as T1 across
//        a module boundary. The substitution env is seeded from
//        `baseTypeRefs[i]` on both sides (#3145 / #2516 wire bit).
//   T5 — IMPORTED generic base, interface substitution: same as T2 across
//        a module boundary.
//   T6 — IMPORTED generic base, OWN generic class with the base parameter
//        in the parent's instantiation (`class Leaf<T>: Base<T>`): the
//        env chains through two levels. Tests the `incoming` parameter of
//        `seedRecordSubstEnv`.
//   T7 — IMPORTED concrete base, control.

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

struct TempFile3171 {
    std::string path;
    explicit TempFile3171(std::string p) : path(std::move(p)) {}
    ~TempFile3171() { std::remove(path.c_str()); }
};

void writeSource3171(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes (matches the umbrella test's convention, #3152):
//   -2  dependency did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end (the §30.14 diagnostic).
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3171 = -2;
constexpr int kAppRejected3171 = -3;

class StderrCapture3171 {
public:
    explicit StderrCapture3171(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3171() { restore(); }
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

bool contains3171(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3171(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3171_");
    TempFile3171 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3171 aObj((dir / (stem + "_a.o")).string());
    writeSource3171(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3171;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3171;

    TempFile3171 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3171 bExe((dir / stem).string());
    writeSource3171(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3171 cap((dir / (stem + ".err")).string());
        ok = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kAppRejected3171;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

int compileAndRun3171(const std::string& src, std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3171L_");
    TempFile3171 s((dir / (stem + ".bzl")).string());
    TempFile3171 exe((dir / stem).string());
    writeSource3171(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3171 cap((dir / (stem + ".err")).string());
        ok = driver.compileFile(s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kAppRejected3171;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — LOCAL GENERIC BASE, PRIMITIVE SUBSTITUTION. `sealed class Leaf:
// Base<int>` must be sendable. Without the fix, REJECTED at the
// `Channel<Leaf>` site — the field `v` is read as bare `T`, the conservative
// reading is "not sendable", and an entirely primitive-typed field is
// refused for a type-system reason.
//
// Value oracle: 44 = 41 (the inherited `v` via the auto-property path) + 3.
// ===========================================================================
BEZEL_TEST(xmod_sendability_generic_base_3171,
           local_sealed_class_generic_base_primitive_admitted_30_14) {
    int local = compileAndRun3171(
        "module Loc3171T1; import System.Threading; "
        "public class Base3171T1<T> "
        "  public v: T; "
        "end Base3171T1; "
        "public sealed class Leaf3171T1: Base3171T1<int> "
        "end Leaf3171T1; "
        "public class C3171T1 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Leaf3171T1>(1); "
        "    using var l := new Leaf3171T1(); "
        "    l.v := 41; "
        "    return l.v + 3; "
        "  end Main; "
        "end C3171T1;");
    if (local == kDepUnavailable3171) return;
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T2 — LOCAL GENERIC BASE, INTERFACE SUBSTITUTION. `sealed class Leaf:
// Base<IShape>` must be REJECTED — the inherited field `v` is interface-typed
// at the `Leaf` use site, and rule 3 forbids interface-typed fields. Without
// the fix, ACCEPTED (the defect's second direction): the field is read as
// bare `T`, the conservative reading is "not sendable", and the verdict
// HAPPENS to be the same — but for the wrong reason. Pinning the diagnostic
// text here is what distinguishes the two paths.
//
// The §14.2 `where T: sendable` route is used (rather than `Channel<T>`)
// because the channel element check emits the flat message without the
// component path; only the constraint route includes the §14.2
// `sendableFailureComponentPath` fragment, which names the offending field
// and the substituted type. The fix is exercised identically on both
// routes — the substitution env is seeded from `rt->genericArgs` and is
// available inside `resolveTypes()` regardless of which call site invokes
// `isSendableType`.
// ===========================================================================
BEZEL_TEST(xmod_sendability_generic_base_3171,
           local_sealed_class_generic_base_interface_rejected_30_14_rule_3) {
    std::string diag;
    int local = compileAndRun3171(
        "module Loc3171T2; "
        "public interface IShape3171T2 "
        "end IShape3171T2; "
        "public class Base3171T2<T> "
        "  public v: T; "
        "end Base3171T2; "
        "public sealed class Leaf3171T2: Base3171T2<IShape3171T2> "
        "end Leaf3171T2; "
        "public class Ferry3171T2<T> where T: sendable "
        "  public property Tag: int := 7; "
        "end Ferry3171T2; "
        "public class C3171T2 "
        "  public static method Main(): int "
        "    using var f := new Ferry3171T2<Leaf3171T2>(); "
        "    return f.Tag; "
        "  end Main; "
        "end C3171T2;",
        &diag);
    if (local == kDepUnavailable3171) return;
    BEZEL_EXPECT_EQ(local, kAppRejected3171);
    // Pin the diagnostic text. The component path names the inherited field
    // and the substituted (interface) type, not the bare `T` — that's the
    // post-fix shape.
    BEZEL_EXPECT_TRUE(contains3171(
        diag, "field `v` has interface type `IShape3171T2` (§30.14 rule 3)"));
}

// ===========================================================================
// T3 — LOCAL CONCRETE BASE, CONTROL. The non-generic base path is unchanged
// by the fix. A concrete base's field types are concrete, so the
// substitution env is empty and the walk proceeds exactly as it did before
// — the fix must not regress this shape.
// ===========================================================================
BEZEL_TEST(xmod_sendability_generic_base_3171,
           local_sealed_class_concrete_base_control_30_14) {
    int local = compileAndRun3171(
        "module Loc3171T3; import System.Threading; "
        "public class Base3171T3 "
        "  public v: int; "
        "end Base3171T3; "
        "public sealed class Leaf3171T3: Base3171T3 "
        "end Leaf3171T3; "
        "public class C3171T3 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Leaf3171T3>(1); "
        "    using var l := new Leaf3171T3(); "
        "    l.v := 41; "
        "    return l.v + 3; "
        "  end Main; "
        "end C3171T3;");
    if (local == kDepUnavailable3171) return;
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T4 — IMPORTED GENERIC BASE, PRIMITIVE SUBSTITUTION. Same as T1 across a
// module boundary. The imported side carries the substituted type via
// `baseTypeRefs[i]->genericArgs` (Symbol.hpp / #3145 / #2516), so the
// substitution env seeds identically to the local side.
// ===========================================================================
BEZEL_TEST(xmod_sendability_generic_base_3171,
           imported_sealed_class_generic_base_primitive_admitted_30_14) {
    int r = compileImportAndRun3171(
        "module Lib3171T4; "
        "public class Base3171T4X<T> "
        "  public v: T; "
        "end Base3171T4X; "
        "public sealed class Leaf3171T4X: Base3171T4X<int> "
        "end Leaf3171T4X;",
        "module App3171T4; import Lib3171T4; import System.Threading; "
        "public class C3171T4X "
        "  public static method Main(): int "
        "    using var ch := new Channel<Leaf3171T4X>(1); "
        "    using var l := new Leaf3171T4X(); "
        "    l.v := 41; "
        "    return l.v + 3; "
        "  end Main; "
        "end C3171T4X;");
    if (r == kDepUnavailable3171) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T5 — IMPORTED GENERIC BASE, INTERFACE SUBSTITUTION. Same as T2 across a
// module boundary. Both halves reject; the imported half pins the same
// diagnostic shape as T2 (substituted interface type, not bare `T`).
//
// Like T2, the §14.2 constraint route is used so the component path is
// emitted (the channel route emits only the flat message).
// ===========================================================================
BEZEL_TEST(xmod_sendability_generic_base_3171,
           imported_sealed_class_generic_base_interface_rejected_30_14_rule_3) {
    std::string diag;
    int r = compileImportAndRun3171(
        "module Lib3171T5; "
        "public interface IShape3171T5X "
        "end IShape3171T5X; "
        "public class Base3171T5X<T> "
        "  public v: T; "
        "end Base3171T5X; "
        "public sealed class Leaf3171T5X: Base3171T5X<IShape3171T5X> "
        "end Leaf3171T5X;",
        "module App3171T5; import Lib3171T5; "
        "public class Ferry3171T5X<T> where T: sendable "
        "  public property Tag: int := 7; "
        "end Ferry3171T5X; "
        "public class C3171T5X "
        "  public static method Main(): int "
        "    using var f := new Ferry3171T5X<Leaf3171T5X>(); "
        "    return f.Tag; "
        "  end Main; "
        "end C3171T5X;",
        &diag);
    if (r == kDepUnavailable3171) return;
    BEZEL_EXPECT_EQ(r, kAppRejected3171);
    BEZEL_EXPECT_TRUE(contains3171(
        diag, "field `v` has interface type `IShape3171T5X` (§30.14 rule 3)"));
}

// ===========================================================================
// T6 — LOCAL GENERIC BASE, OWN GENERIC CLASS. `Leaf<T>: Base<T>`: when the
// local `Leaf<int>` walks `Base<T>`'s field, the inherited walker must
// chain the env through `Leaf`'s substitution. Tests the `incoming`
// argument to `seedRecordSubstEnv`. Without the fix, the inherited walk
// reads the field as bare `T`; with the fix, as `int`.
//
// LOCAL ONLY because the importer cannot construct an imported generic
// class whose generic base also lives in the imported module (#711 —
// separate gap; not in scope here).
// ===========================================================================
BEZEL_TEST(xmod_sendability_generic_base_3171,
           local_sealed_class_generic_base_with_own_param_chained_env) {
    int local = compileAndRun3171(
        "module Loc3171T6; import System.Threading; "
        "public class Base3171T6<T> "
        "  public v: T; "
        "end Base3171T6; "
        "public sealed class Leaf3171T6<T>: Base3171T6<T> "
        "end Leaf3171T6<T>; "
        "public class C3171T6 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Leaf3171T6<int>>(1); "
        "    using var l := new Leaf3171T6<int>(); "
        "    l.v := 41; "
        "    return l.v + 3; "
        "  end Main; "
        "end C3171T6;");
    if (local == kDepUnavailable3171) return;
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T7 — IMPORTED CONCRETE BASE, CONTROL. Non-generic base, across the
// boundary. Identical to T3 but on the imported side. Empty env, the fix
// must produce the same verdict as the umbrella test's pre-existing base-
// inheritance cases.
// ===========================================================================
BEZEL_TEST(xmod_sendability_generic_base_3171,
           imported_sealed_class_concrete_base_control_30_14) {
    int r = compileImportAndRun3171(
        "module Lib3171T7; "
        "public class Base3171T7X "
        "  public v: int; "
        "end Base3171T7X; "
        "public sealed class Leaf3171T7X: Base3171T7X "
        "end Leaf3171T7X;",
        "module App3171T7; import Lib3171T7; import System.Threading; "
        "public class C3171T7X "
        "  public static method Main(): int "
        "    using var ch := new Channel<Leaf3171T7X>(1); "
        "    using var l := new Leaf3171T7X(); "
        "    l.v := 41; "
        "    return l.v + 3; "
        "  end Main; "
        "end C3171T7X;");
    if (r == kDepUnavailable3171) return;
    BEZEL_EXPECT_EQ(r, 44);
}

}  // namespace bezel::module
