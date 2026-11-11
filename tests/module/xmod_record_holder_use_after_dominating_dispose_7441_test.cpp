// narrow #7441 (§28.2 "Use after a dominating explicit disposal", §9.7; v0.357)
// — the record-holder rule ACROSS A MODULE BOUNDARY.
//
// §28.2 (v0.357): "The same error applies to a use through a local borrow of
// that variable — ... a borrow-bearing record, choice or span (§9.7) ... — when
// every value in the holder's source set (K2.2) has been so disposed on every
// path to the use." Nothing in the sentence depends on WHERE the record type is
// declared, so `var p := new Pair(i); i.Dispose(); p.Item!.Name` is the same
// error when `Pair` comes from an imported module.
//
// WHY THIS FILE EXISTS. An imported type has no `ast::TypeDecl` in the
// importing compilation (`Symbol::decl` is null for every import — umbrella
// #3152), so a rule that asks the AST "is this a positional record with no
// declared constructor?" silently answers "no" for every import and the holder
// is never tracked: the single-module program is rejected and its two-module
// twin compiles and reads freed memory. The record arms of
// `MethodOwnership::bindHeldBorrows` therefore read both facts off the SYMBOL
// (`sema::recordPositionalFieldSymbols`, a `sema::ConstructorSymbol` member).
// Each reject row below pairs the imported program with its single-module twin
// and requires the SAME diagnostic from both.
//
// MEASURED on the base compiler (ad9596cee) with these shapes printing
// `Name`: T1's and T2's imported programs compile, exit 0 and print an EMPTY
// line where the admitting twin prints the item's name — the read goes
// through a disposed instance.
//
// The admitting twins (T3, T4) are the over-rejection controls: a disposal
// after the holder's last use, and a disposal on one branch only (§28.2: such
// a holder "is not rejected"), both through the imported record.

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
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

struct TempFile7441X {
    std::string path;
    explicit TempFile7441X(std::string p) : path(std::move(p)) {}
    ~TempFile7441X() { std::remove(path.c_str()); }
};

void writeSource7441X(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes, chosen so an admitted program's own exit status can never be
// confused with a rejected compile:
//   -2  the DEPENDENCY did not compile (asserted against in every row: the
//       library is emitted by `compileObject`, which needs no external
//       toolchain, so this can only mean the library source is wrong).
//   -3  the app was REJECTED by the front-end.
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepFailed7441X = -2;
constexpr int kRejected7441X = -3;

// `CompileResult` carries only a bool + a count and the front-end prints its
// diagnostics to stderr, so stderr is captured around the compile and every
// reject row pins the actual diagnostic text.
class StderrCapture7441X {
public:
    explicit StderrCapture7441X(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture7441X() { restore(); }
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

bool contains7441X(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The declarations under test. `tag` keeps module and type names unique per
// row; the same text is used as the library module and, prefixed to the app's
// class, as the single-module twin.
std::string decls7441X(const std::string& tag) {
    return "public class Item" + tag + " "
           "  private _name: string; "
           "  public constructor(name: string) "
           "    _name := name; "
           "  end constructor; "
           "  public property Name: string => _name; "
           "end Item" + tag + "; "
           "public record Pair" + tag + "(Item: Item" + tag + "?); ";
}

std::string app7441X(const std::string& tag, const std::string& members) {
    return "public class App" + tag + " " + members + " end App" + tag + ";";
}

// Two modules: the declarations in a library compiled with `compileObject`,
// the app importing it.
int compileImportAndRun7441X(const std::string& tag, const std::string& members,
                             std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz7441x_");
    TempFile7441X aSrc((dir / (stem + "_a.bzl")).string());
    TempFile7441X aObj((dir / (stem + "_a.o")).string());
    writeSource7441X(aSrc.path, "module XLib7441" + tag + "; " + decls7441X(tag));
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepFailed7441X;
    }
    if (!std::ifstream(aObj.path).good()) return kDepFailed7441X;

    TempFile7441X bSrc((dir / (stem + "_b.bzl")).string());
    TempFile7441X bExe((dir / stem).string());
    writeSource7441X(bSrc.path, "module XApp7441" + tag + "; import XLib7441" +
                                    tag + "; " + app7441X(tag, members));
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture7441X cap((dir / (stem + ".err")).string());
        // compileFileForTest, not driver.compileFile: the whole-suite ASan
        // sweep only reaches hand-rolled runners through this wrapper
        // (tests/support/driver_run_support.hpp).
        ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                 .success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected7441X;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module twin: the SAME declarations and the same app class in one
// compilation.
int compileAndRun7441X(const std::string& tag, const std::string& members,
                       std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz7441xL_");
    TempFile7441X s((dir / (stem + ".bzl")).string());
    TempFile7441X exe((dir / stem).string());
    writeSource7441X(s.path, "module XLoc7441" + tag + "; " + decls7441X(tag) +
                                 app7441X(tag, members));
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture7441X cap((dir / (stem + ".err")).string());
        ok = bezel::test::compileFileForTest(driver, s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected7441X;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

const char* kHolderNeedle7441X = "after a dominating explicit Dispose() of `i`";

}  // namespace

// ===========================================================================
// T1 — the issue's own shape (#7441 probe p05) with the record IMPORTED.
// §28.2 (v0.357): the record holder's only source is disposed on every path to
// the use, so the use is a compile-time error — in the two-module program
// exactly as in its single-module twin.
// ===========================================================================
BEZEL_TEST(xmod_record_holder_use_after_dominating_dispose_7441,
           imported_record_holder_member_access_rejected_spec_28_2) {
    const std::string tag = "T1";
    const std::string members =
        "public static method Main(): int "
        "  var i := new Item" + tag + "(\"i\"); "
        "  var p := new Pair" + tag + "(i); "
        "  i.Dispose(); "
        "  return p.Item!.Name.Length; "
        "end Main;";
    std::string diag;
    int r = compileImportAndRun7441X(tag, members, &diag);
    BEZEL_EXPECT_TRUE(r != kDepFailed7441X);
    BEZEL_EXPECT_EQ(r, kRejected7441X);
    BEZEL_EXPECT_TRUE(contains7441X(diag, kHolderNeedle7441X));
    BEZEL_EXPECT_TRUE(contains7441X(diag, "`p` is a borrow-bearing record"));

    std::string twinDiag;
    int twin = compileAndRun7441X(tag, members, &twinDiag);
    BEZEL_EXPECT_EQ(twin, kRejected7441X);
    BEZEL_EXPECT_TRUE(contains7441X(twinDiag, kHolderNeedle7441X));
    BEZEL_EXPECT_TRUE(contains7441X(twinDiag, "`p` is a borrow-bearing record"));
}

// ===========================================================================
// T2 — an alias of a COMPONENT of the imported record (`var x := p.Item;`).
// The component is one of the borrows the record carries (§9.7), so the alias
// borrows what the record borrows; §28.2 (v0.357) rejects its use after the
// dominating disposal. The component's positional-ness is read off the symbol
// too, so the imported program answers as its twin does.
// ===========================================================================
BEZEL_TEST(xmod_record_holder_use_after_dominating_dispose_7441,
           alias_of_an_imported_record_component_rejected_spec_28_2) {
    const std::string tag = "T2";
    const std::string members =
        "public static method Main(): int "
        "  var i := new Item" + tag + "(\"i\"); "
        "  var p := new Pair" + tag + "(i); "
        "  var x := p.Item; "
        "  i.Dispose(); "
        "  return x!.Name.Length; "
        "end Main;";
    std::string diag;
    int r = compileImportAndRun7441X(tag, members, &diag);
    BEZEL_EXPECT_TRUE(r != kDepFailed7441X);
    BEZEL_EXPECT_EQ(r, kRejected7441X);
    BEZEL_EXPECT_TRUE(contains7441X(diag, kHolderNeedle7441X));
    BEZEL_EXPECT_TRUE(contains7441X(diag, "use of `x`"));

    std::string twinDiag;
    int twin = compileAndRun7441X(tag, members, &twinDiag);
    BEZEL_EXPECT_EQ(twin, kRejected7441X);
    BEZEL_EXPECT_TRUE(contains7441X(twinDiag, kHolderNeedle7441X));
    BEZEL_EXPECT_TRUE(contains7441X(twinDiag, "use of `x`"));
}

// ===========================================================================
// T3 — the admitting twin of T1: the disposal comes AFTER the holder's last
// use. §28.2 (v0.357) rejects a use the kill dominates; here no use follows
// the kill, so the program compiles and runs (41 = "i".Length + 40).
// ===========================================================================
BEZEL_TEST(xmod_record_holder_use_after_dominating_dispose_7441,
           imported_record_holder_disposal_after_last_use_accepted_spec_28_2) {
    const std::string tag = "T3";
    const std::string members =
        "public static method Main(): int "
        "  var i := new Item" + tag + "(\"i\"); "
        "  var p := new Pair" + tag + "(i); "
        "  var n := p.Item!.Name.Length; "
        "  i.Dispose(); "
        "  return n + 40; "
        "end Main;";
    std::string diag;
    int r = compileImportAndRun7441X(tag, members, &diag);
    BEZEL_EXPECT_TRUE(r != kDepFailed7441X);
    BEZEL_EXPECT_EQ(r, 41);
    BEZEL_EXPECT_TRUE(!contains7441X(diag, "after a dominating explicit"));

    int twin = compileAndRun7441X(tag, members);
    BEZEL_EXPECT_EQ(twin, 41);
}

// ===========================================================================
// T4 — the over-rejection control: the disposal is on ONE branch only, and the
// branch is not taken at run time. §28.2 (v0.357): a holder whose source was
// disposed on only some paths "is not rejected" (the checked build's runtime
// fault is the backstop), so the imported program compiles and returns 41.
// ===========================================================================
BEZEL_TEST(xmod_record_holder_use_after_dominating_dispose_7441,
           imported_record_holder_kill_on_one_branch_accepted_spec_28_2) {
    const std::string tag = "T4";
    const std::string members =
        "public static method Run(kill: bool): int "
        "  var i := new Item" + tag + "(\"i\"); "
        "  var p := new Pair" + tag + "(i); "
        "  if kill then "
        "    i.Dispose(); "
        "  end; "
        "  return p.Item!.Name.Length + 40; "
        "end Run; "
        "public static method Main(): int "
        "  return App" + tag + ".Run(false); "
        "end Main;";
    std::string diag;
    int r = compileImportAndRun7441X(tag, members, &diag);
    BEZEL_EXPECT_TRUE(r != kDepFailed7441X);
    BEZEL_EXPECT_EQ(r, 41);
    BEZEL_EXPECT_TRUE(!contains7441X(diag, "after a dominating explicit"));

    int twin = compileAndRun7441X(tag, members);
    BEZEL_EXPECT_EQ(twin, 41);
}

}  // namespace bezel::module
