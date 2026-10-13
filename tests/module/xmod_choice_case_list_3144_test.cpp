// Spec-derived regression test for issue #3144 — family F9 of the
// imported-symbol classification seam (umbrella #3152).
//
// spec §35.1 (choice declaration) / §35.3 (construction and payload ownership)
// / §35.4 (the classification) / §35.5 (matching) / §35.7 (equality) /
// §35.12 (sendability) / §2.5 (imported declarations) / §2.8.2–§2.8.3 (module
// artifacts), v0.254. Audit:
// docs/proposals/papers/imported-symbol-audit-2026-08-13.md, family F9.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `MetaTypeDeclKind::Choice` crossed the module boundary; the CASE LIST did
// not. The schema said so in its own comment ("the case-list itself is not yet
// serialized"). So an imported choice arrived carrying `kind == Choice` and
// ZERO cases, and `ModuleLoader` built no `ChoiceCaseSymbol` at all.
//
// That is the third shape of the 2026-08-06 taxonomy — not a wrong predicate,
// but a CORRECT PREDICATE FED AN EMPTY SOURCE — and it is harder to see than
// the other families precisely because every individual predicate is right:
//
//     std::optional<ChoiceClassification> classifyChoiceCore(...) {
//         const auto* decl = dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
//         if (!decl) return std::nullopt;      // null for EVERY import
//         for (const auto& cc : decl->choiceCases) ...
//
// ── MEASURED, with real two-module builds ──────────────────────────────────
//   (`bezelc -c lib.bzl -o lib.o` then `bezelc --import lib.o app.bzl`)
//
// The triage predicted a leak. The leak is real, but it is not the headline:
// the headline is a silent ABI MISMATCH, the #2950 class. A choice is a VALUE
// type returned BY VALUE, and its size decides whether the ABI returns it in
// registers or through a hidden sret pointer:
//
//     exporter  %choice.Outcome = type { i32, { ptr }, { i8 }, { i32 } }
//     importer  %choice.Outcome = type { i32 }
//
// so the importer called the dependency's own compiled function without the
// sret pointer it expects:
//
//     AddressSanitizer: SEGV on unknown address 0x8 ... WRITE
//       #0 bz$LibF9.Maker$MakeFetched
//
//   arm (base 137f58f -> this fix)                imported        local twin
//   -------------------------------------------  --------------  ----------
//   owning choice returned across the boundary    SIGSEGV -> 5    5 / 5
//     (destructor-counted disposals of 5 payloads)
//   payload-free case inferred (`var q := Msg.Quiet`)
//                                                 reject -> 1     1 / 1
//   generic choice payload (`Opt<int>.Some(3)`)    reject -> 3     3 / 3
//   `=` on a value-like choice, admissible payload accept -> 1     1 / 1
//   `=` on a value-like choice, INADMISSIBLE      wrong reject     §35.7
//     payload                                     -> §35.7        reject
//
// With the WIRE carriage alone and the classifier still AST-gated (an
// intermediate build), the ABI matched and the program ran — and LeakSanitizer
// reported `199 of 200` owned payloads never disposed. That isolates the second
// half of the fix: the §35.4 classifier had to go symbol-side too.
//
// ── WHY EVERY ARM HAS A LOCAL TWIN ─────────────────────────────────────────
//
// Each arm compiles the SAME program text twice: once split across a module
// boundary, once as a single module. The twin is the differential control — it
// passed before the fix and must keep passing — so what an arm pins is not "the
// program works" but "the boundary changed the answer".
//
// ── THE OVER-REJECTION CONTROL ─────────────────────────────────────────────
//
// One site pulled the other way. `isAutoEqualityBearingChoice`
// (SymbolCollection.cpp) answered `true` for every imported choice without
// examining a single payload — an over-ACCEPT. Making it a real check can only
// REJECT more, so `eq_admissible_payload_still_accepted` pins the accept
// direction and `eq_inadmissible_payload_rejected_like_local` pins that the new
// rejection is the SAME §35.7 rejection the local twin already gave.

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3144 {
    std::string path;
    explicit TempFile3144(std::string p) : path(std::move(p)) {}
    ~TempFile3144() { std::remove(path.c_str()); }
};

void writeSource3144(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Distinct sentinels, because this family's pre-fix behaviour spans BOTH a
// compile reject and a runtime SIGSEGV. Folding them together would let a
// crashing arm pass as "rejected".
constexpr int kDepFailed3144 = -2;   // the exporter itself failed
constexpr int kRejected3144 = -1;    // the IMPORTER did not compile
// A signalled child is reported as 1000 + signal, so 1011 == SIGSEGV — the
// exact pre-fix result of the ABI mismatch, and a value no arm's expected exit
// code can collide with.
constexpr int kSignalBase3144 = 1000;

int runChild3144(const std::string& exe) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exe.c_str(), exe.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return kSignalBase3144 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : kRejected3144;
}

// Compile `libSrc` to an object, compile `appSrc` against it, run the app in a
// separate process. `capturedStderr`, when non-null, receives the importer's
// stderr — the only way an arm can assert on diagnostic TEXT, which matters
// here because a bare "did not compile" cannot tell the §35.7 rejection this
// fix INTRODUCES from the "no member `Held` on type `Wrap`" rejection it
// REPLACES.
int compileImportAndRun3144(const std::string& libSrc, const std::string& appSrc,
                            const std::string& tag,
                            std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3144_" + tag + "_" + unique_stem("");
    TempFile3144 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3144 lObj((dir / (stem + "_lib.o")).string());
    writeSource3144(lSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(lSrc.path, lObj.path).success)
            return kDepFailed3144;
    }
    if (!std::ifstream(lObj.path).good()) return kDepFailed3144;

    TempFile3144 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile3144 aExe((dir / stem).string());
    TempFile3144 aErr((dir / (stem + ".err")).string());
    writeSource3144(aSrc.path, appSrc);

    bezel::Driver driver;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath = lObj.path;
    std::vector<bezel::Dependency> deps{d};

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
        compiled =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path, deps)
                .success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(aErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path, deps)
                .success;
    }
    if (!compiled) return kRejected3144;
    if (!std::ifstream(aExe.path).good()) return kRejected3144;
    return runChild3144(aExe.path);
}

// The LOCAL twin: the dependency's declarations and the app body in ONE module.
int compileAndRun3144(const std::string& src, const std::string& tag,
                      std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3144L_" + tag + "_" + unique_stem("");
    TempFile3144 aSrc((dir / (stem + ".bzl")).string());
    TempFile3144 aExe((dir / stem).string());
    TempFile3144 aErr((dir / (stem + ".err")).string());
    writeSource3144(aSrc.path, src);
    bezel::Driver driver;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
        compiled =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(aErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success;
    }
    if (!compiled) return kRejected3144;
    if (!std::ifstream(aExe.path).good()) return kRejected3144;
    return runChild3144(aExe.path);
}

// ── The dependency ─────────────────────────────────────────────────────────
//
// `Res._disposed` is the LIVE-OBJECT COUNTER: the destructor bumps a
// process-global that `Main` reads back, so "the active case's `owned` payload
// was disposed" is an ASSERTION, not an inference from "it did not crash".
//
// The four choices cover the three §35.4 classifications plus the generic form:
//   Outcome — OWNING   (`owned` class payload, §35.4 rule 1)
//   Msg     — VALUE-LIKE (payload-free + int + string, §35.4 rule 3)
//   View    — BORROW-BEARING (unmarked class payload, §35.4 rule 2)
//   Opt<T>  — GENERIC (a bare type-parameter payload, §35.11)
const char* kDecls3144 =
    "public class Res "
    "  public static _disposed: int := 0; "
    "  public v: int; "
    "  public constructor(v0: int) v := v0; end constructor; "
    "  destructor Res._disposed := Res._disposed + 1; end destructor; "
    "  public static method GetDisposed(): int => Res._disposed; "
    "end Res; "
    "public choice Outcome "
    "  Fetched(owned response: Res), "
    "  NotModified, "
    "  Failed(status: int); "
    "end Outcome; "
    "public choice Msg "
    "  Quiet, "
    "  Code(n: int), "
    "  Note(text: string); "
    "end Msg; "
    "public choice View "
    "  Empty, "
    "  Borrowed(r: Res); "
    "end View; "
    "public choice Opt<T> "
    "  None, "
    "  Some(value: T); "
    "end Opt; "
    "public class Maker "
    "  public static method MakeFetched(v: int): Outcome "
    "    return Outcome.Fetched(new Res(v)); "
    "  end MakeFetched; "
    "end Maker; ";

std::string lib3144() {
    return std::string("module ChoiceLib3144; ") + kDecls3144;
}

std::string importer3144(const std::string& body) {
    return "module ChoiceApp3144; import ChoiceLib3144; class C3144 " + body +
           " end C3144; ";
}

std::string local3144(const std::string& body) {
    return std::string("module ChoiceApp3144; ") + kDecls3144 + "class C3144 " +
           body + " end C3144; ";
}

bool mentions3144(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ───────────────────────────────────────────────────────────────────────────
// 1. The headline: an OWNING choice crossing the boundary by value.
//
// Two facts in one number. The program returns the DISPOSAL COUNT, so:
//   * reaching `Main` at all proves the importer's `%choice.Outcome` layout
//     matches the exporter's (pre-fix: SIGSEGV inside the dependency's own
//     `MakeFetched`, because a 4-byte reading of the return type omitted the
//     sret pointer the 24-byte exporter passes);
//   * the value 5 proves §35.4's "Disposal is defined by the active case" ran
//     for each of the five constructed values (pre-fix, once the ABI was
//     repaired but the classifier still AST-gated: 199 of 200 leaked under
//     LSan).
// ───────────────────────────────────────────────────────────────────────────

namespace {
const char* kOwningBody3144 =
    "  static method Work(): int "
    "    var i := 0; "
    "    while i < 5 do "
    "      var o := Maker.MakeFetched(i); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Work; "
    "  public static method Main(): int "
    "    var q := C3144.Work(); "
    "    return Res.GetDisposed(); "
    "  end Main; ";
}  // namespace

BEZEL_TEST(xmod_choice_case_list_3144,
           imported_owning_choice_disposes_active_case_payload_35_4) {
    const int r = compileImportAndRun3144(lib3144(),
                                          importer3144(kOwningBody3144), "own");
    BEZEL_EXPECT_TRUE(r != kDepFailed3144);
    BEZEL_EXPECT_TRUE(r != kRejected3144);
    // 1011 == SIGSEGV: the pre-fix result. Named explicitly so a regression
    // reads as "the ABI mismatch is back", not as an off-by-N.
    BEZEL_EXPECT_TRUE(r != kSignalBase3144 + 11);
    BEZEL_EXPECT_EQ(r, 5);
}

BEZEL_TEST(xmod_choice_case_list_3144,
           local_owning_choice_disposes_active_case_payload_35_4) {
    // The differential control: same text, one module. Passed before the fix.
    BEZEL_EXPECT_EQ(compileAndRun3144(local3144(kOwningBody3144), "own"), 5);
}

// ───────────────────────────────────────────────────────────────────────────
// 2. §35.3: a PAYLOAD-FREE case access infers the choice type.
//
// `var q := Msg.Quiet;` must give `q` the type `Msg`, or codegen's
// `choiceTypeOf` finds no choice for the `case` scrutinee and every arm falls
// through to the #1847 "not supported by the backend" reject. The payload CALL
// form (`Msg.Code(41)`) is typed by `typeCall` and was NOT affected — which is
// why only the payload-free spelling diverged, and why it needs its own arm.
// ───────────────────────────────────────────────────────────────────────────

namespace {
const char* kInferBody3144 =
    "  public static method Main(): int "
    "    var q := Msg.Quiet; "
    "    return (case q of Quiet => 1, Code(_) => 0, Note(_) => 0); "
    "  end Main; ";
}  // namespace

BEZEL_TEST(xmod_choice_case_list_3144,
           imported_payload_free_case_access_infers_choice_type_35_3) {
    BEZEL_EXPECT_EQ(
        compileImportAndRun3144(lib3144(), importer3144(kInferBody3144), "inf"),
        1);
}

BEZEL_TEST(xmod_choice_case_list_3144,
           local_payload_free_case_access_infers_choice_type_35_3) {
    BEZEL_EXPECT_EQ(compileAndRun3144(local3144(kInferBody3144), "inf"), 1);
}

// ───────────────────────────────────────────────────────────────────────────
// 3. §35.11 / §35.5: a GENERIC choice's bare-type-parameter payload binds at
// the INSTANTIATION. `Some(x)` over `Opt<int>` must bind `x: int`; binding it
// at the bare `T` made the two arms have no common type (§20) — a false
// reject, since the local twin compiles.
// ───────────────────────────────────────────────────────────────────────────

namespace {
const char* kGenericBody3144 =
    "  public static method Main(): int "
    "    var g := Opt<int>.Some(3); "
    "    return (case g of None => 0, Some(x) => x); "
    "  end Main; ";
}  // namespace

BEZEL_TEST(xmod_choice_case_list_3144,
           imported_generic_choice_payload_binds_at_instantiation_35_11) {
    BEZEL_EXPECT_EQ(
        compileImportAndRun3144(lib3144(), importer3144(kGenericBody3144), "gen"),
        3);
}

BEZEL_TEST(xmod_choice_case_list_3144,
           local_generic_choice_payload_binds_at_instantiation_35_11) {
    BEZEL_EXPECT_EQ(compileAndRun3144(local3144(kGenericBody3144), "gen"), 3);
}

// ───────────────────────────────────────────────────────────────────────────
// 4. §35.4 rule 2 / §9.7: a BORROW-BEARING imported choice built from a NAMED
// local. The classification has to be reachable at all for the §9.7 borrow
// rules to apply to an imported choice; this arm pins that the value works and
// its borrow is still readable through the match.
// ───────────────────────────────────────────────────────────────────────────

namespace {
const char* kBorrowBody3144 =
    "  public static method Main(): int "
    "    var r := new Res(7); "
    "    var v := View.Borrowed(r); "
    "    var n := (case v of Empty => 0, Borrowed(x) => x.v); "
    "    r.Dispose(); "
    "    return n; "
    "  end Main; ";
}  // namespace

BEZEL_TEST(xmod_choice_case_list_3144,
           imported_borrow_bearing_choice_reads_its_payload_35_4) {
    BEZEL_EXPECT_EQ(
        compileImportAndRun3144(lib3144(), importer3144(kBorrowBody3144), "bor"),
        7);
}

BEZEL_TEST(xmod_choice_case_list_3144,
           local_borrow_bearing_choice_reads_its_payload_35_4) {
    BEZEL_EXPECT_EQ(compileAndRun3144(local3144(kBorrowBody3144), "bor"), 7);
}

// ───────────────────────────────────────────────────────────────────────────
// 5. THE OVER-REJECTION CONTROL, both directions (§35.7 / §9.2).
//
// `isAutoEqualityBearingChoice` used to `return true` for an imported choice
// with its payloads never examined. Turning that into a real walk can only
// reject MORE, so both directions need pinning:
//   (a) an admissible payload must still be ACCEPTED — and give the local
//       twin's answer;
//   (b) an inadmissible payload must be rejected with the SAME §35.7
//       diagnostic the local twin already gives — not with the structural
//       "no member `Held` on type `Wrap`" the pre-fix build produced, which
//       was a rejection for the wrong reason and would keep a coarse
//       "rejects()" assertion green.
// ───────────────────────────────────────────────────────────────────────────

namespace {
// A combined-form record: the body field disqualifies §9.2 auto-equality, so
// `Body` is NOT an admissible equality component — while still being a VALUE,
// which is what makes `Wrap` value-like and so reachable by the §35.7 gate.
const char* kEqDecls3144 =
    "public choice Tag "
    "  Zero, "
    "  One(n: int), "
    "  Two(s: string); "
    "end Tag; "
    "public record Body(a: int) "
    "  private _b: int := 0; "
    "end Body; "
    "public choice Wrap "
    "  Empty, "
    "  Held(b: Body); "
    "end Wrap; ";

std::string eqLib3144() {
    return std::string("module EqChoiceLib3144; ") + kEqDecls3144;
}
std::string eqImporter3144(const std::string& body) {
    return "module EqChoiceApp3144; import EqChoiceLib3144; class C3144 " +
           body + " end C3144; ";
}
std::string eqLocal3144(const std::string& body) {
    return std::string("module EqChoiceApp3144; ") + kEqDecls3144 +
           "class C3144 " + body + " end C3144; ";
}

const char* kEqOkBody3144 =
    "  public static method Main(): int "
    "    var a := Tag.One(5); "
    "    var b := Tag.One(5); "
    "    if a = b then return 1; end; "
    "    return 2; "
    "  end Main; ";

const char* kEqBadBody3144 =
    "  public static method Main(): int "
    "    var a := Wrap.Held(new Body(1)); "
    "    var b := Wrap.Held(new Body(1)); "
    "    if a = b then return 1; end; "
    "    return 2; "
    "  end Main; ";
}  // namespace

BEZEL_TEST(xmod_choice_case_list_3144,
           eq_admissible_payload_still_accepted_35_7) {
    BEZEL_EXPECT_EQ(
        compileImportAndRun3144(eqLib3144(), eqImporter3144(kEqOkBody3144),
                                "eqok"),
        1);
    BEZEL_EXPECT_EQ(compileAndRun3144(eqLocal3144(kEqOkBody3144), "eqok"), 1);
}

BEZEL_TEST(xmod_choice_case_list_3144,
           eq_inadmissible_payload_rejected_like_local_35_7) {
    std::string importedErr;
    const int ri = compileImportAndRun3144(
        eqLib3144(), eqImporter3144(kEqBadBody3144), "eqbad", &importedErr);
    BEZEL_EXPECT_EQ(ri, kRejected3144);
    // The rejection must be the §35.7 auto-hashability one, not the pre-fix
    // structural "no member" — the whole point of the arm.
    BEZEL_EXPECT_TRUE(mentions3144(importedErr, "not auto-hashable"));
    BEZEL_EXPECT_TRUE(mentions3144(importedErr, "\xC2\xA7"
                                                "35.7"));
    BEZEL_EXPECT_TRUE(!mentions3144(importedErr, "no member `Held`"));

    std::string localErr;
    const int rl =
        compileAndRun3144(eqLocal3144(kEqBadBody3144), "eqbad", &localErr);
    BEZEL_EXPECT_EQ(rl, kRejected3144);
    BEZEL_EXPECT_TRUE(mentions3144(localErr, "not auto-hashable"));
}

// ───────────────────────────────────────────────────────────────────────────
// 6. §35.12 / §14.2 sendability — the arm that pins `Sendability.cpp`'s check
// as REAL rather than fail-safe.
//
// That site carried `if (!td) return false;`, a deliberate fail-safe added
// while F9 was open: with no case list there was nothing to walk, so trusting
// the value-like verdict would have admitted an UNEXAMINED choice across a
// domain boundary. It is now a real per-payload walk, and a real walk has to be
// pinned in BOTH directions:
//   (a) a value-like imported choice whose payloads are all sendable SATISFIES
//       `where T: sendable` (§14.2 / §30.14) — the fail-safe answered "not
//       sendable" here, a FALSE REJECT of a program whose local twin compiles;
//   (b) a value-like imported choice with an `Object` payload — value-like by
//       §35.4 rule 3, NOT sendable by §35.12 — is still REJECTED.
//
// (b) asserts on the diagnostic's REASON CLAUSE, and that is the point: a
// conservative default can produce the verdict but cannot produce
// "payload `o` of type `Object` is not sendable (§35.12)", which only the
// per-payload walk can. MEASURED on base 137f58f the imported half printed the
// bare constraint failure with NO reason for both choices; the local twin
// printed the full clause. After the fix the imported and local diagnostics are
// identical.
//
// The obvious spelling for (a) would have been `Domain.Spawn(Entry, value)`.
// It is NOT used: a choice-typed `Domain.Spawn` argument fails the LLVM IR
// verifier ("Invalid bitcast … %choice.SendOk … to i64") identically in the
// imported and the LOCAL build, i.e. a pre-existing §30 codegen gap unrelated
// to this family. `where T: sendable` runs the same §30.14 predicate through
// §14.2 without that path.
// ───────────────────────────────────────────────────────────────────────────

namespace {
const char* kSendDecls3144 =
    "public choice SendOk "
    "  Nothing, "
    "  Num(n: int), "
    "  Text(s: string); "
    "end SendOk; "
    "public choice SendBad "
    "  Nothing, "
    "  Any(o: Object); "
    "end SendBad; ";

std::string sendLib3144() {
    return std::string("module SendChoiceLib3144; ") + kSendDecls3144;
}

// `where T: sendable` (§14.2 derived-property constraint) runs the §30.14 /
// §35.12 sendability predicate at the instantiation.
std::string sendBody3144(const char* choiceTy, const char* ctor) {
    return std::string(
               "class Box3144 "
               "  public static method Hold<T>(x: T): int where T: sendable "
               "    => 1; "
               "end Box3144; "
               "class C3144 public static method Main(): int "
               "  return Box3144.Hold<") + choiceTy + ">(" + ctor + "); "
           "end Main; end C3144; ";
}

std::string sendImporter3144(const char* choiceTy, const char* ctor) {
    return "module SendChoiceApp3144; import SendChoiceLib3144; " +
           sendBody3144(choiceTy, ctor);
}
std::string sendLocal3144(const char* choiceTy, const char* ctor) {
    return std::string("module SendChoiceApp3144; ") + kSendDecls3144 +
           sendBody3144(choiceTy, ctor);
}
}  // namespace

BEZEL_TEST(xmod_choice_case_list_3144,
           imported_value_like_choice_satisfies_sendable_constraint_35_12) {
    // fix #3144 (review nit N5): `err` was declared, captured, and never
    // asserted on. Rather than delete it — this is the ACCEPT arm of the
    // over-rejection control, and the thing worth pinning is that it accepts
    // for the right reason — assert the stderr is free of the §35.12 rejection
    // its sibling arm expects. Without this, the arm would still pass if the
    // program were accepted while emitting an unrelated diagnostic.
    std::string err;
    const int r = compileImportAndRun3144(
        sendLib3144(), sendImporter3144("SendOk", "SendOk.Num(1)"), "sendok",
        &err);
    BEZEL_EXPECT_TRUE(r != kDepFailed3144);
    BEZEL_EXPECT_EQ(r, 1);
    BEZEL_EXPECT_TRUE(err.find("is not sendable") == std::string::npos);
    // The LOCAL twin: accepted before the fix too — that is what made the
    // imported rejection a divergence rather than a rule.
    BEZEL_EXPECT_EQ(compileAndRun3144(sendLocal3144("SendOk", "SendOk.Num(1)"),
                                      "sendok"),
                    1);
}

BEZEL_TEST(xmod_choice_case_list_3144,
           imported_choice_with_object_payload_still_not_sendable_35_12) {
    std::string err;
    const int r = compileImportAndRun3144(
        sendLib3144(), sendImporter3144("SendBad", "SendBad.Any(1)"), "sendbad",
        &err);
    BEZEL_EXPECT_EQ(r, kRejected3144);
    BEZEL_EXPECT_TRUE(mentions3144(err, "does not satisfy the constraint"));
    // The REASON CLAUSE — only a real per-payload walk can produce it.
    BEZEL_EXPECT_TRUE(mentions3144(err, "payload `o` of type `Object` is not "
                                        "sendable"));

    std::string localErr;
    const int rl = compileAndRun3144(sendLocal3144("SendBad", "SendBad.Any(1)"),
                                     "sendbad", &localErr);
    BEZEL_EXPECT_EQ(rl, kRejected3144);
    BEZEL_EXPECT_TRUE(mentions3144(localErr, "payload `o` of type `Object` is "
                                             "not sendable"));
}

}  // namespace bezel::module
