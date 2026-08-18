// fix #3235 (§30.14 / §14.2, spec v0.254): the sibling to the #3165 call-site
// fix on the §14.2 CONSTRAINT route (`where T: sendable`).
//
// #3165 closed the §30.14 call-site route (Channel<T>, Domain.Spawn) for a
// captured primary-ctor parameter of non-sendable type — at the call site
// `FieldSymbol::posParamBodyRef` is set by ExprTypecheck's walk, so the
// §30.14 rule-2 storage enumeration sees the captured parameter as storage
// and rejects. The §14.2 constraint route, however, runs inside
// `resolveTypes()`, BEFORE ExprTypecheck sets `posParamBodyRef` — so reading
// the bit there answers "uncaptured" for every class. A captured primary-ctor
// parameter of a non-sendable type therefore passes `where T: sendable` on
// BOTH sides of a module boundary. MEASURED on `eeed89e`:
//
//     public sealed class Bad(s: IShape?)
//       public method Peek(): bool => s <> null;   -- `s` IS body-referenced =>
//     end Bad;                                     -- §22.10 captures it as a field
//
//     public class Ferry<T> where T: sendable
//       public property Tag: int := 3;
//     end Ferry;
//
//     public class App
//       public static method Main(): int
//         var f := new Ferry<Bad>();                 -- ACCEPTED at eeed89e
//         return f.Tag;
//       end Main;
//     end App;
//
// `s` is body-referenced in `Peek`, so §22.10 captures it as a private field.
// The `where T: sendable` constraint on `Ferry`'s type parameter T is a §14.2
// derived-property constraint naming the §30.14 sendable derived-property.
// Satisfaction is decided per instantiation by the property's defining
// computation (`isSendableType`), which walks storage. For a class, captured
// primary-ctor parameters ARE storage; uncaptured ones are not. The check
// used to run inside `resolveTypes()`, so `posParamBodyRef` was always
// `false` at the check site — the captured parameter was invisible, the
// walk answered "no storage of non-sendable type", `Bad` was admitted, and
// the constraint ACCEPTED `Ferry<Bad>`.
//
// The fix defers the §14.2 sendable verdict (via
// `SemaContext::pendingSendableConstraints_`) and replays it at the tail of
// `typecheckExpressions()`, AFTER ExprTypecheck's expression walk has set
// `posParamBodyRef`. By that point the bit is populated, the storage walk
// sees the captured parameter of non-sendable type, and `Bad` is correctly
// rejected — `Ferry<Bad>` becomes a compile error. The pattern mirrors
// `checkClassSerializationEligibilityLate` (#2325) and rides inside
// `typecheckExpressions()` itself so every existing and future pipeline
// (the driver AND every test-harness construction — there are dozens)
// gets it for free.
//
// WHAT EACH TEST IS FOR (axis grid: LOCAL+IMPORTED × captured+not-captured,
// §14.2 constraint route only — the §30.14 call-site route is covered by
// the sibling #3165 test):
//
//   T1 — LOCAL captured POSITIVE: a captured `owned l: Sendable` is admitted
//        under rule 2. The constraint `where T: sendable` is satisfied.
//        Value oracle proves the runtime shape. PASSES both before and
//        after the fix; the role of T1 is the GUARD that the late-pass
//        replay accepts what the in-place check accepted, and does not
//        regress the captured-sendable verdict.
//   T2 — LOCAL captured NEGATIVE: a captured `s: IShape?` makes the class
//        NOT sendable. `Ferry<Bad>` is REJECTED. WITHOUT the fix, ACCEPTED
//        (the accept-hole this lane closes).
//   T3 — LOCAL UNCAPTURED POSITIVE: an uncaptured primary-ctor parameter
//        (used only in an initializer position — §22.10 "not captured")
//        is not storage. The class is sendable. PASSES both before and
//        after the fix; the role of T3 is the GUARD that the late-pass
//        replay does NOT turn this into a "captured" verdict universally.
//   T4 — IMPORTED captured POSITIVE: same as T1 across a module boundary.
//   T5 — IMPORTED captured NEGATIVE: same as T2 across the boundary. The
//        wire bit `MetaMember::posParamBodyRef` (#3221) carries the
//        capture classification across the boundary, so the imported
//        storage walk sees the captured parameter exactly as the local
//        side does.
//   T6 — IMPORTED UNCAPTURED POSITIVE: same as T3 across the boundary.
//
// Each reject pins the DIAGNOSTIC TEXT (via stderr capture) rather than
// just the reject, so a later syntax slip in the inline source cannot
// keep it green for the wrong reason (the #3152 / #3165 guard pattern).

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

struct TempFile3235 {
    std::string path;
    explicit TempFile3235(std::string p) : path(std::move(p)) {}
    ~TempFile3235() { std::remove(path.c_str()); }
};

void writeSource3235(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes (matches the sibling #3165 test's convention):
//   -2  dependency did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end (the §14.2 diagnostic).
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3235 = -2;
constexpr int kAppRejected3235 = -3;

class StderrCapture3235 {
public:
    explicit StderrCapture3235(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3235() { restore(); }
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

bool contains3235(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3235(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3235_");
    TempFile3235 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3235 aObj((dir / (stem + "_a.o")).string());
    writeSource3235(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3235;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3235;

    TempFile3235 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3235 bExe((dir / stem).string());
    writeSource3235(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3235 cap((dir / (stem + ".err")).string());
        ok = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kAppRejected3235;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

int compileAndRun3235(const std::string& src, std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3235L_");
    TempFile3235 s((dir / (stem + ".bzl")).string());
    TempFile3235 exe((dir / stem).string());
    writeSource3235(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3235 cap((dir / (stem + ".err")).string());
        ok = driver.compileFile(s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kAppRejected3235;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — LOCAL CAPTURED POSITIVE: a captured `owned l: Leaf3235T1` (§22.10) of
// a sendable class type. Rule 2 admits "an `owned` field whose declared type
// is a sendable class type". The class IS sendable, so `Ferry<Node3235T1>`
// (where `where T: sendable`) is satisfied. Value oracle proves the runtime
// shape is unchanged.
//
// This test PASSES BOTH BEFORE AND AFTER the fix: pre-fix the in-place
// constraint check answered "sendable" by skipping the captured parameter
// (posParamBodyRef was false at resolveTypes time), post-fix the late replay
// arrives at the same verdict by seeing the captured parameter as an
// `owned`-sendable storage member. The role of T1 is the GUARD — the
// late-pass replay must not reject what the in-place check accepted.
// ===========================================================================
BEZEL_TEST(xmod_sendability_constraint_3235,
           local_captured_owned_primary_ctor_field_admitted_14_2) {
    int local = compileAndRun3235(
        "module Loc3235T1; "
        "public sealed class Leaf3235T1 "
        "  public property V: int := 41; "
        "end Leaf3235T1; "
        "public sealed class Node3235T1(owned l: Leaf3235T1) "
        "  public method Get(): int => l.V; "
        "end Node3235T1; "
        "public class Ferry<T> where T: sendable "
        "  public method Tag(): int => 3; "
        "end Ferry; "
        "public class C3235T1 "
        "  public static method Main(): int "
        "    var f := new Ferry<Node3235T1>(); "
        "    return f.Tag() + 3; "
        "  end Main; "
        "end C3235T1;");
    if (local == kDepUnavailable3235) return;
    BEZEL_EXPECT_EQ(local, 6);
}

// ===========================================================================
// T2 — LOCAL CAPTURED NEGATIVE: a captured `s: IShape3235T2?` (§22.10) of a
// nullable interface type. Per §30.14 rule 3, a class with an
// interface-typed field is NOT sendable. The §14.2 `where T: sendable`
// constraint on `Ferry`'s T must REJECT `Ferry<Bad3235T2>`. WITHOUT the
// fix, ACCEPTED (the accept-hole this lane closes on the §14.2 route).
// ===========================================================================
BEZEL_TEST(xmod_sendability_constraint_3235,
           local_captured_interface_field_rejected_30_14_rule_3) {
    std::string diag;
    int local = compileAndRun3235(
        "module Loc3235T2; "
        "public interface IShape3235T2 "
        "end IShape3235T2; "
        "public sealed class Bad3235T2(s: IShape3235T2?) "
        "  public method Peek(): bool => s <> null; "
        "end Bad3235T2; "
        "public class Ferry<T> where T: sendable "
        "  public method Tag(): int => 3; "
        "end Ferry; "
        "public class C3235T2 "
        "  public static method Main(): int "
        "    var f := new Ferry<Bad3235T2>(); "
        "    return f.Tag(); "
        "  end Main; "
        "end C3235T2;",
        &diag);
    if (local == kDepUnavailable3235) return;
    BEZEL_EXPECT_EQ(local, kAppRejected3235);
    // Pin the REASON, not just the failure: a future syntax slip in the
    // source above would otherwise keep this test green. The diagnostic
    // carries the §14.2 citation plus, when the walk confidently pins a
    // single component, a §3.6 rule-4 component-path fragment naming the
    // captured field (`s`) and its interface type (§30.14 rule 3).
    BEZEL_EXPECT_TRUE(contains3235(diag,
        "does not satisfy the constraint `where T: sendable` (§14.2)"));
    BEZEL_EXPECT_TRUE(contains3235(diag, "IShape3235T2"));
    BEZEL_EXPECT_TRUE(contains3235(diag, "§30.14 rule 3"));
}

// ===========================================================================
// T3 — LOCAL UNCAPTURED POSITIVE: a primary-ctor parameter that is referenced
// ONLY in an initializer position (§22.10 "not captured") is NOT a field, so
// the class is sendable regardless of the parameter's type. The fix must
// not turn this into a captured-parameter walk universally — the late
// replay must continue to skip uncaptured parameters exactly as the
// in-place check did. `s` is consumed by the initializer of `_label` and
// is never used in a body position; `n` IS captured (body-referenced in
// `N()`) but its type is a primitive and so cannot make the class
// non-sendable on its own.
// ===========================================================================
BEZEL_TEST(xmod_sendability_constraint_3235,
           local_uncaptured_primary_ctor_param_is_not_a_field_22_10) {
    int local = compileAndRun3235(
        "module Loc3235T3; "
        "public sealed class Box3235T3(s: string, n: int) "
        "  private _label: string := s; "
        "  public method N(): int => n; "
        "end Box3235T3; "
        "public class Ferry<T> where T: sendable "
        "  public method Tag(): int => 3; "
        "end Ferry; "
        "public class C3235T3 "
        "  public static method Main(): int "
        "    var f := new Ferry<Box3235T3>(); "
        "    var b := new Box3235T3(\"hi\", 41); "
        "    return f.Tag() + b.N(); "
        "  end Main; "
        "end C3235T3;");
    if (local == kDepUnavailable3235) return;
    BEZEL_EXPECT_EQ(local, 44);
}

// ===========================================================================
// T4 — IMPORTED CAPTURED POSITIVE: same as T1 across a module boundary. The
// wire bit `MetaMember::posParamBodyRef` (#3221) carries the capture
// classification across the wire, so the imported side's storage walk
// sees the captured parameter of sendable type exactly as the local side
// does. Constructed through an explicit convenience constructor because an
// imported class's IMPLICIT primary constructor is not callable across a
// module boundary.
// ===========================================================================
BEZEL_TEST(xmod_sendability_constraint_3235,
           imported_captured_owned_primary_ctor_field_admitted_14_2) {
    int r = compileImportAndRun3235(
        "module Lib3235T4; "
        "public sealed class Leaf3235T4X "
        "  public property V: int := 41; "
        "end Leaf3235T4X; "
        "public sealed class Node3235T4X(owned l: Leaf3235T4X) "
        "  public method Get(): int => l.V; "
        "  public constructor() "
        "    this(new Leaf3235T4X()); "
        "  end constructor; "
        "end Node3235T4X; "
        "public class Ferry<T> where T: sendable "
        "  public method Tag(): int => 3; "
        "end Ferry;",
        "module App3235T4; import Lib3235T4; "
        "public class C3235T4X "
        "  public static method Main(): int "
        "    var f := new Ferry<Node3235T4X>(); "
        "    return f.Tag() + 3; "
        "  end Main; "
        "end C3235T4X;");
    if (r == kDepUnavailable3235) return;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T5 — IMPORTED CAPTURED NEGATIVE: same as T2 across a module boundary.
// The accept-hole was on BOTH sides — local AND imported — so this pins
// the imported reject too. The imported half uses the same wire bit
// (#3221 `MetaMember::posParamBodyRef`) the local half uses, AND the
// late pass at the tail of `typecheckExpressions()` runs on the
// importer's compilation too — so the importer's expression walk has
// already set `posParamBodyRef` on the imported class's primary-ctor
// FieldSymbol when the late pass replays the §14.2 verdict.
// ===========================================================================
BEZEL_TEST(xmod_sendability_constraint_3235,
           imported_captured_interface_field_rejected_30_14_rule_3) {
    std::string diag;
    int r = compileImportAndRun3235(
        "module Lib3235T5; "
        "public interface IShape3235T5X "
        "end IShape3235T5X; "
        "public sealed class Bad3235T5X(s: IShape3235T5X?) "
        "  public method Peek(): bool => s <> null; "
        "  public constructor() "
        "    this(null); "
        "  end constructor; "
        "end Bad3235T5X; "
        "public class Ferry<T> where T: sendable "
        "  public method Tag(): int => 3; "
        "end Ferry;",
        "module App3235T5; import Lib3235T5; "
        "public class C3235T5X "
        "  public static method Main(): int "
        "    var f := new Ferry<Bad3235T5X>(); "
        "    return f.Tag(); "
        "  end Main; "
        "end C3235T5X;",
        &diag);
    if (r == kDepUnavailable3235) return;
    BEZEL_EXPECT_EQ(r, kAppRejected3235);
    BEZEL_EXPECT_TRUE(contains3235(diag,
        "does not satisfy the constraint `where T: sendable` (§14.2)"));
    BEZEL_EXPECT_TRUE(contains3235(diag, "IShape3235T5X"));
    BEZEL_EXPECT_TRUE(contains3235(diag, "§30.14 rule 3"));
}

// ===========================================================================
// T6 — IMPORTED UNCAPTURED POSITIVE: same as T3 across a module boundary.
// An uncaptured parameter is not a field on the IMPORTED side either, so
// the class is sendable and `Ferry<Box3235T6X>` is satisfied.
// ===========================================================================
BEZEL_TEST(xmod_sendability_constraint_3235,
           imported_uncaptured_primary_ctor_param_is_not_a_field_22_10) {
    int r = compileImportAndRun3235(
        "module Lib3235T6; "
        "public sealed class Box3235T6X(s: string, n: int) "
        "  private _label: string := s; "
        "  public method N(): int => n; "
        "  public constructor() "
        "    this(\"hi\", 41); "
        "  end constructor; "
        "end Box3235T6X; "
        "public class Ferry<T> where T: sendable "
        "  public method Tag(): int => 3; "
        "end Ferry;",
        "module App3235T6; import Lib3235T6; "
        "public class C3235T6X "
        "  public static method Main(): int "
        "    var f := new Ferry<Box3235T6X>(); "
        "    var b := new Box3235T6X(); "
        "    return f.Tag() + b.N(); "
        "  end Main; "
        "end C3235T6X;");
    if (r == kDepUnavailable3235) return;
    BEZEL_EXPECT_EQ(r, 44);
}

}  // namespace bezel::module
