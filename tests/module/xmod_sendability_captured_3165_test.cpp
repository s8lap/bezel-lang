// fix #3165 (§30.14 rule 2 / §22.10 / §2.8.2 clause 1, spec v0.254):
// §30.14 rule 2's storage enumeration deliberately skipped a CLASS's
// primary-ctor parameters entirely — even captured ones — because the capture
// bit (`FieldSymbol::posParamBodyRef`) is computed by ExprTypecheck's
// expression walk, AFTER `resolveTypes()` finishes (the §14.2 constraint
// route sits inside `resolveTypes()`). Reading the bit during `resolveTypes()`
// would answer "uncaptured" for every class on that route, producing an
// ORDER-DEPENDENT verdict.
//
// This was the right call while #3152 was the only seam in flight: a captured
// parameter's storage type was NEVER WEIGHED on either side of a module
// boundary. MEASURED on the umbrella commit's base:
//
//     public sealed class Bad(s: IShape?)
//       public method Greet(): string
//         var x: IShape? := s;
//         if x = null then return "hi"; else return x.ToString();
//         end if;
//       end Greet;
//     end Bad;
//
//     public class App
//       public static method Main(): int
//         using var ch := new Channel<Bad>(1);
//         using var b := new Bad(null);
//         return 1;
//       end Main;
//     end App;
//
// — `s` is body-referenced in `Greet`, so §22.10 captures it as an `owned`
// private field. The `Channel<Bad>` element check (a §30.14 call-site rule)
// must therefore REJECT `Bad`: an interface-typed captured field violates
// rule 3's "no interface-typed fields". The pre-#3165 walk did not weigh
// `s` at all, so `Bad` was accepted — an ACCEPT-HOLE on BOTH sides of the
// module boundary, an instance moving across a domain boundary holding an
// origin-domain interface reference (a data race).
//
// #3221 (already merged into this lane) bridged `posParamBodyRef` across the
// wire as `MetaMember::posParamBodyRef` (the 18th trailing scalar) so the
// imported side has the same answer as the local side. This commit closes
// the local/imported accept-hole on the §30.14 CALL-SITE route (the
// §30.15 / Domain.Spawn site, where the bit IS set by the time the check
// runs). The §14.2 constraint route inside `resolveTypes()` still reads
// `false` and is therefore unchanged — closing that is a separate late-pass
// job (filed alongside, the umbrella's noted follow-up).
//
// WHAT EACH TEST IS FOR (axis grid: LOCAL+IMPORTED × captured+not-captured):
//
//   T1 — LOCAL captured POSITIVE: a captured `owned l: Sendable` is admitted
//        under rule 2. Without the fix, the walk would skip `l` and either
//        the test's runtime oracle (44) or the rule 2/3 verdict would change.
//        The control is the §22.10 capture rule itself, not a verdict flip.
//   T2 — LOCAL captured NEGATIVE: a captured `s: IShape?` makes the class
//        not sendable. REJECTED. Without the fix, ACCEPTED (the accept-hole).
//   T3 — LOCAL UNCAPTURED POSITIVE: a primary-ctor parameter used ONLY in
//        an initializer position (§22.10 "not captured") is not a field; the
//        class is sendable. REJECTED-when-not-sendable stays false.
//   T4 — IMPORTED captured POSITIVE: same as T1 but constructed across a
//        module boundary. This is the side #3221's wire bit enables.
//   T5 — IMPORTED captured NEGATIVE: same as T2 but across the boundary.
//        The accept-hole was on BOTH sides, so both halves pin the reject.
//   T6 — IMPORTED UNCAPTURED POSITIVE: same as T3 across the boundary.
//
// Each reject pins the DIAGNOSTIC TEXT (via stderr capture) rather than just
// the reject, so a later syntax slip in the inline source cannot keep it
// green for the wrong reason (the #3152 T5-T8 guard pattern).

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

struct TempFile3165 {
    std::string path;
    explicit TempFile3165(std::string p) : path(std::move(p)) {}
    ~TempFile3165() { std::remove(path.c_str()); }
};

void writeSource3165(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes (matches the umbrella test's convention, #3152):
//   -2  dependency did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end (the §30.14 diagnostic).
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3165 = -2;
constexpr int kAppRejected3165 = -3;

class StderrCapture3165 {
public:
    explicit StderrCapture3165(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3165() { restore(); }
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

bool contains3165(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3165(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3165_");
    TempFile3165 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3165 aObj((dir / (stem + "_a.o")).string());
    writeSource3165(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3165;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3165;

    TempFile3165 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3165 bExe((dir / stem).string());
    writeSource3165(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3165 cap((dir / (stem + ".err")).string());
        ok = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kAppRejected3165;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

int compileAndRun3165(const std::string& src, std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3165L_");
    TempFile3165 s((dir / (stem + ".bzl")).string());
    TempFile3165 exe((dir / stem).string());
    writeSource3165(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3165 cap((dir / (stem + ".err")).string());
        ok = driver.compileFile(s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kAppRejected3165;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — LOCAL CAPTURED POSITIVE: a captured `owned l: Leaf` (§22.10) of a
// sendable class type. Rule 2 admits "an `owned` field whose declared type is
// a sendable class type". The class IS sendable. Value oracle proves the
// imported storage was carried correctly across the channel.
//
// This test PASSES BOTH BEFORE AND AFTER the fix: pre-fix it was already
// sendable on the local side because the umbrella fix #3152 closed the
// local/imported divergence for the `owned` marker. Its job here is the
// GUARD — the fix must not regress the already-correct local verdict, and
// must not change the storage's runtime behaviour.
// ===========================================================================
BEZEL_TEST(xmod_sendability_captured_3165,
           local_captured_owned_primary_ctor_field_admitted_30_14) {
    int local = compileAndRun3165(
        "module Loc3165T1; import System.Threading; "
        "public sealed class Leaf3165T1 "
        "  public property V: int := 41; "
        "end Leaf3165T1; "
        "public sealed class Node3165T1(owned l: Leaf3165T1) "
        "  public method Get(): int => l.V; "
        "end Node3165T1; "
        "public class C3165T1 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Node3165T1>(1); "
        "    using var n := new Node3165T1(new Leaf3165T1()); "
        "    return n.Get() + 3; "
        "  end Main; "
        "end C3165T1;");
    if (local == kDepUnavailable3165) return;
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T2 — LOCAL CAPTURED NEGATIVE: a captured `s: IShape?` (§22.10). Per §30.14
// rule 3, a class with an interface-typed field is NOT sendable. Without the
// fix, the walk SKIPS `s` entirely (it was the "captured primary-ctor param"
// shape), so the class is falsely admitted — the accept-hole that #3165
// fixes. REJECTED on the local side after the fix.
// ===========================================================================
BEZEL_TEST(xmod_sendability_captured_3165,
           local_captured_interface_field_rejected_30_14_rule_3) {
    std::string diag;
    int local = compileAndRun3165(
        "module Loc3165T2; import System.Threading; "
        "public interface IShape3165T2 "
        "end IShape3165T2; "
        "public sealed class Bad3165T2(s: IShape3165T2?) "
        "  public method Greet(): string "
        "    var x: IShape3165T2? := s; "
        "    if x = null then "
        "      return \"hi\"; "
        "    else "
        "      return x.ToString(); "
        "    end if; "
        "  end Greet; "
        "end Bad3165T2; "
        "public class C3165T2 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Bad3165T2>(1); "
        "    using var b := new Bad3165T2(null); "
        "    return 1; "
        "  end Main; "
        "end C3165T2;",
        &diag);
    if (local == kDepUnavailable3165) return;
    BEZEL_EXPECT_EQ(local, kAppRejected3165);
    // Pin the REASON, not just the failure: a future syntax slip in the
    // source above would otherwise keep this test green.
    BEZEL_EXPECT_TRUE(contains3165(diag, "`Bad3165T2` of `Channel<T>` is not sendable"));
}

// ===========================================================================
// T3 — LOCAL UNCAPTURED POSITIVE: a primary-ctor parameter that is referenced
// ONLY in an initializer position is NOT a field (§22.10), so it cannot make
// the class non-sendable. The local walk must continue to skip uncaptured
// parameters — i.e. the fix must not turn this into a captured-parameter
// walk universally. REJECTED-when-not-sendable stays false.
//
// `s` is consumed by the initializer of `_label` and is never used in a
// body position. `n` is captured (body-referenced in `N()`), but its type is
// a primitive and so cannot make the class non-sendable on its own.
// ===========================================================================
BEZEL_TEST(xmod_sendability_captured_3165,
           local_uncaptured_primary_ctor_param_is_not_a_field_22_10) {
    int local = compileAndRun3165(
        "module Loc3165T3; import System.Threading; "
        "public sealed class Box3165T3(s: string, n: int) "
        "  private _label: string := s; "
        "  public method N(): int => n; "
        "end Box3165T3; "
        "public class C3165T3 "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3165T3>(1); "
        "    using var b := new Box3165T3(\"hi\", 41); "
        "    return b.N() + 3; "
        "  end Main; "
        "end C3165T3;");
    if (local == kDepUnavailable3165) return;
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T4 — IMPORTED CAPTURED POSITIVE: same shape as T1 across a module boundary.
// This is the side the umbrella fix #3221 enables — without the wire bit
// the imported side answers "uncaptured" for every class and the runtime
// shape is wrong. Constructed through an explicit convenience constructor
// because an imported class's IMPLICIT primary constructor is not callable
// across a module boundary (unrelated to sendability, the umbrella's #3166).
// ===========================================================================
BEZEL_TEST(xmod_sendability_captured_3165,
           imported_captured_owned_primary_ctor_field_admitted_30_14) {
    int r = compileImportAndRun3165(
        "module Lib3165T4; "
        "public sealed class Leaf3165T4X "
        "  public property V: int := 41; "
        "end Leaf3165T4X; "
        "public sealed class Node3165T4X(owned l: Leaf3165T4X) "
        "  public method Get(): int => l.V; "
        "  public constructor() "
        "    this(new Leaf3165T4X()); "
        "  end constructor; "
        "end Node3165T4X;",
        "module App3165T4; import Lib3165T4; import System.Threading; "
        "public class C3165T4X "
        "  public static method Main(): int "
        "    using var ch := new Channel<Node3165T4X>(1); "
        "    using var n := new Node3165T4X(); "
        "    return n.Get() + 3; "
        "  end Main; "
        "end C3165T4X;");
    if (r == kDepUnavailable3165) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T5 — IMPORTED CAPTURED NEGATIVE: same shape as T2 across a module
// boundary. The accept-hole was on BOTH sides — local AND imported — so
// this pins the imported reject. The imported half uses the same wire bit
// (#3221 `MetaMember::posParamBodyRef`) the local half uses.
// ===========================================================================
BEZEL_TEST(xmod_sendability_captured_3165,
           imported_captured_interface_field_rejected_30_14_rule_3) {
    std::string diag;
    int r = compileImportAndRun3165(
        "module Lib3165T5; "
        "public interface IShape3165T5X "
        "end IShape3165T5X; "
        "public sealed class Bad3165T5X(s: IShape3165T5X?) "
        "  public method Greet(): string "
        "    var x: IShape3165T5X? := s; "
        "    if x = null then "
        "      return \"hi\"; "
        "    else "
        "      return x.ToString(); "
        "    end if; "
        "  end Greet; "
        "  public constructor() "
        "    this(null); "
        "  end constructor; "
        "end Bad3165T5X;",
        "module App3165T5; import Lib3165T5; import System.Threading; "
        "public class C3165T5X "
        "  public static method Main(): int "
        "    using var ch := new Channel<Bad3165T5X>(1); "
        "    using var b := new Bad3165T5X(); "
        "    return 1; "
        "  end Main; "
        "end C3165T5X;",
        &diag);
    if (r == kDepUnavailable3165) return;
    BEZEL_EXPECT_EQ(r, kAppRejected3165);
    BEZEL_EXPECT_TRUE(contains3165(diag, "`Bad3165T5X` of `Channel<T>` is not sendable"));
}

// ===========================================================================
// T6 — IMPORTED UNCAPTURED POSITIVE: same shape as T3 across a module
// boundary. An uncaptured parameter is not a field on the IMPORTED side
// either, so the class is sendable and the test runs.
// ===========================================================================
BEZEL_TEST(xmod_sendability_captured_3165,
           imported_uncaptured_primary_ctor_param_is_not_a_field_22_10) {
    int r = compileImportAndRun3165(
        "module Lib3165T6; "
        "public sealed class Box3165T6X(s: string, n: int) "
        "  private _label: string := s; "
        "  public method N(): int => n; "
        "  public constructor() "
        "    this(\"hi\", 41); "
        "  end constructor; "
        "end Box3165T6X;",
        "module App3165T6; import Lib3165T6; import System.Threading; "
        "public class C3165T6X "
        "  public static method Main(): int "
        "    using var ch := new Channel<Box3165T6X>(1); "
        "    using var b := new Box3165T6X(); "
        "    return b.N() + 3; "
        "  end Main; "
        "end C3165T6X;");
    if (r == kDepUnavailable3165) return;
    BEZEL_EXPECT_EQ(r, 44);
}

}  // namespace bezel::module
