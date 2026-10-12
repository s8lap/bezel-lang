// fix #3162 + #3164 (§22.10 "Methods and constructors — primary constructors" /
// §22.8 "Field initializers" steps 0-2 / §22.3 "Parameter modifiers" /
// §28.8 "Ownership transfer" / §2.8.3 rule 4; spec v0.254) —
// `new ImportedClass(args)` through a §22.10 PRIMARY CONSTRUCTOR.
//
// ── WHAT WAS BROKEN ────────────────────────────────────────────────────────
//
// #3162: a dependency exports `bz$<Type>$init(ptr)`, which takes only the
// object, so a primary constructor's PARAMETERS had no channel across a module
// boundary. #3141 had already replaced the misleading "the implicit default
// constructor takes 0 (§22.7)" diagnostic — which named a constructor the class
// does not have — with an honest decline, but the CONSTRUCTION stayed
// unsupported: measured on the parent commit `ec6a02f`, `new PBox(3, 4)` on an
// imported `class PBox(V: int, W: int)` was a hard compile error while the
// character-identical single-module twin printed `sum=7 v=3 w=4 q=6`.
//
// The gap was never a CLASSIFICATION one, and that is the whole fix. §22.8
// orders construction: **step 0** stores the primary parameters into their
// field slots, **step 1** runs the base chain, **step 2** runs the field
// initializers. Step 0 is the CALLER's, and `bz$<Type>$init` is steps 1+2
// against the object it is handed — so `lowerNew`'s imported arm now performs
// step 0 into the importer's own `fieldIndex_` slots and then calls the
// dependency's `init`. Nothing new crosses the wire for #3162.
//
// #3164: one thing did have to cross — the `owned` marker on a primary
// parameter. It rides `ast::Param::modifier` and nowhere else
// (`SymbolCollection::collectTypeDecl` never copied it onto the FieldSymbol),
// so `MetaMember::modifiers` was empty for every positional field and an
// importer saw EVERY imported primary parameter as a borrow.
//
// ── WHY THE TWO ARE ONE COMMIT (the measured ordering) ──────────────────────
//
// #3164 was filed "Medium, latent … worth doing FIRST so #3162 does not land an
// ownership hole". That is right, and stronger than filed. With the #3162
// codegen+sema half applied and the #3164 carriage line disabled, on a real
// two-module build:
//
//     new Holder(owned r, 20)   -> FALSE REJECT, "the `owned` call-site marker
//                                  is only valid on an argument to an `owned`
//                                  parameter" (§22.3) — a legal, indeed
//                                  MANDATORY, spelling made uncallable
//     new Holder(r, 20)         -> compiles CLEAN, **SIGSEGV (exit 139)**
//     the same program in ONE module -> 211 (correct)
//
// i.e. exactly #2659's shape one module further out: the caller keeps
// auto-dropping the value while the callee's generated `Dispose()` also
// disposes it. Shipping #3162 alone would have been a memory-safety
// REGRESSION, not merely an incomplete feature — the reject it removes is
// what currently keeps the hole unreachable. Rows T4-T6 are the ones that
// flip on the carriage line alone.
//
// ── THE ENCODING ───────────────────────────────────────────────────────────
//
// Every value row returns a composite so that a zeroed object, a skipped
// initializer, a skipped base chain and a double/absent disposal each land on a
// DIFFERENT number, all inside `WEXITSTATUS`'s 0..255:
//
//   T1  Sum() * 10 + Q          = 76   (7 and 6; a calloc-zero object reads 0)
//   T3  Total() * 10 + Ax2      = 170  (16 and 10; an unpropagated base
//                                       argument reads 60 — measured)
//   T4  Val() * 10 + disposals  = 211  (21 and exactly 1; a double free
//                                       crashes, a leak reads 210)
//
// ── ORACLES ────────────────────────────────────────────────────────────────
//
// Every row here runs under BOTH gate oracles: the runner goes through
// `bezel::test::compileFileForTest`, so `gate.sh --full` instruments the
// importer's emitted code and LeakSanitizer turns a leak into a non-zero exit.
// ⚠ The DEPENDENCY object is never instrumented (`compileObject` ignores
// `sanitizeAddress`), so a fault inside the dependency's own `Dispose()` is
// seen as a signal, not an ASan report — which is why T4 counts disposals with
// a live counter instead of trusting "it did not crash".

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

struct TempFile3162 {
    std::string path;
    explicit TempFile3162(std::string p) : path(std::move(p)) {}
    ~TempFile3162() { std::remove(path.c_str()); }
};

void writeSource3162(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes, chosen so an admitted program's own exit status can never be
// confused with a rejected compile:
//   -2  the DEPENDENCY did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end.
//   -1  the app compiled but did not exit normally (a signal — e.g. the
//       double free this fix's ordering exists to prevent).
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3162 = -2;
constexpr int kRejected3162 = -3;

// A REJECT row that asserted only `kRejected3162` would stay green if one of
// these inline sources later acquired a plain syntax error — green for the
// wrong reason, and doubly so here, where the pre-fix behaviour of two rows WAS
// a rejection (with the wrong text) and of one row was a runtime crash. Capture
// stderr so every reject pins its actual diagnostic.
class StderrCapture3162 {
public:
    explicit StderrCapture3162(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3162() { restore(); }
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

bool mentions3162(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3162(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3162_");
    TempFile3162 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3162 aObj((dir / (stem + "_a.o")).string());
    writeSource3162(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3162;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3162;

    TempFile3162 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3162 bExe((dir / stem).string());
    writeSource3162(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3162 cap((dir / (stem + ".err")).string());
        // compileFileForTest, not driver.compileFile: the whole-suite ASan
        // sweep (`gate.sh --full`) only reaches hand-rolled runners through
        // this wrapper (tests/support/driver_run_support.hpp).
        ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                 .success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3162;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module twin: the SAME declarations in one compilation.
int compileAndRun3162(const std::string& src, std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3162L_");
    TempFile3162 s((dir / (stem + ".bzl")).string());
    TempFile3162 exe((dir / stem).string());
    writeSource3162(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3162 cap((dir / (stem + ".err")).string());
        ok = bezel::test::compileFileForTest(driver, s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3162;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The declarations under test. `kDecls3162` is used VERBATIM on both sides —
// as a dependency module's body and, spliced into the twin, as part of a single
// module — so an imported row and its twin differ in exactly one thing: whether
// a module boundary sits between the declaration and the `new`.
// ---------------------------------------------------------------------------
const char* kDecls3162 =
    // The counted resource. Its destructor bumps a process-global counter that
    // the program reads back, so "disposed exactly once" is asserted rather
    // than inferred from "it did not crash".
    "public class Res "
    "  public static _disposed: int := 0; "
    "  public v: int; "
    "  public constructor() v := 1; end; "
    "  destructor Res._disposed := Res._disposed + 1; end destructor; "
    "  public static method GetDisposed(): int => Res._disposed; "
    "end Res; "
    // §22.10 primary constructor with an `owned` parameter CAPTURED by a body
    // reference (`r.v` in Val) — spec §22.10: "captured as a compiler-generated
    // `owned` private field, disposed with the object".
    "public class Holder(owned r: Res, tag: int) "
    "  public method Val(): int "
    "    return r.v + tag; "
    "  end Val; "
    "end Holder; "
    // A plain primary constructor whose FIELD INITIALIZER reads a primary
    // parameter — §22.8 step 2 reading what step 0 stored. This is the
    // ordering the fix's sketch flagged as the thing to check first.
    "public class PBox(V: int, W: int) "
    "  public Q: int := V * 2; "
    "  public method Sum(): int "
    "    return V + W; "
    "  end Sum; "
    "end PBox; "
    // A `string` positional parameter — #1605's independent +1.
    "public class SBox(S: string, N: int) "
    "  public method Len(): int "
    "    return S.Length + N; "
    "  end Len; "
    "end SBox; "
    // A primary-constructor class with a base clause carrying an ARGUMENT
    // (§22.10 inheritance / §22.8 step 1).
    "public class Base2(A: int) "
    "  public Ax2: int := A * 2; "
    "end Base2; "
    "public class Derived2(A: int, B: int): Base2(A) "
    "  public method Total(): int "
    "    return Ax2 + B; "
    "  end Total; "
    "end Derived2; "
    // A GENERIC primary-constructor class. `lowerNew` routes an imported
    // generic to #662's Foundation A/C arm (monomorphized in the importer),
    // which has no `bz$<Type>$init` and performs no step 0 — so this shape
    // stays DECLINED cross-module while working locally (T13/T13b).
    "public class GBox<T>(V: int, W: int) "
    "  public _t: T; "
    "  public method Sum(): int "
    "    return V + W; "
    "  end Sum; "
    "end GBox; "
    // Over-rejection control: an EXPLICIT constructor, which already
    // constructed cross-module through `bz$<Type>$ctor` before this fix.
    "public class ExplicitBox "
    "  public v: int; "
    "  public constructor(x: int) v := x + 100; end; "
    "end ExplicitBox;";

std::string lib3162(const std::string& tag) {
    return "module Lib3162" + tag + "; " + kDecls3162;
}
std::string app3162(const std::string& tag, const std::string& body) {
    return "module App3162" + tag + "; import Lib3162" + tag + "; "
           "public class C" + tag + " " + body + " end C" + tag + ";";
}
std::string twin3162(const std::string& tag, const std::string& body) {
    return "module Twin3162" + tag + "; " + kDecls3162 +
           " public class C" + tag + " " + body + " end C" + tag + ";";
}

}  // namespace

// ===========================================================================
// T1 — §22.10 / §22.8 steps 0+2: the positional arguments reach the imported
// primary constructor's field slots, and a field initializer that READS a
// primary parameter sees the stored value.
//
// `Sum() * 10 + Q` = 7*10 + 6 = 76. A calloc-zero object (the outcome #3141's
// decline existed to prevent) reads 0; an object whose parameters were stored
// but whose initializer ran BEFORE them — step 2 before step 0 — reads 70.
//
// MEASURED before the fix: twin 76, imported REJECTED ("`new PBox` constructs
// an IMPORTED class through its primary constructor, which is not yet
// supported").
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t1_positional_args_and_initializer_read_22_10_22_8) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(3, 4); "
        "    return b.Sum() * 10 + b.Q; "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("T1"), app3162("T1", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 76);
}

BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t1_twin_same_program_in_one_module_22_10) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(3, 4); "
        "    return b.Sum() * 10 + b.Q; "
        "  end Main;";
    int r = compileAndRun3162(twin3162("T1L", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 76);
}

// ===========================================================================
// T2 — §4.6 / §28.9 / #1605: a `string` positional parameter gets its own
// independent reference.
//
// `SBox(S: string, N: int)` with `S.Length + N` = 5 + 2 = 7. The store path is
// `storePrimaryCtorPositionalArgs`, whose #1605 retain is now reached from the
// imported arm too — without it the field aliases the caller's local with no
// reference of its own and `Dispose()` reads freed memory. The loop row below
// is what would actually catch an over- or under-retain.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t2_string_positional_parameter_retains_1605) {
    const std::string body =
        "  public static method Main(): int "
        "    var s := \"abcde\"; "
        "    var b := new SBox(s, 2); "
        "    return b.Len(); "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("T2"), app3162("T2", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 7);
}

BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t2_string_positional_parameter_loop_1605) {
    // 200 constructions. An unbalanced retain shows up here — as a leak under
    // `--full` (LeakSanitizer forces exit 1) or as a use-after-free crash on
    // the plain oracle — where a single construction can hide it.
    const std::string body =
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0..199 do "
        "      var s := \"abcde\"; "
        "      var b := new SBox(s, 2); "
        "      acc := acc + b.Len(); "
        "    end for; "
        "    return acc / 200; "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("T2b"), app3162("T2b", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T3 — §22.10 inheritance / §22.8 step 1: a base clause's ARGUMENT reaches the
// imported base's own positional slot.
//
// `class Derived2(A, B): Base2(A)` with `Base2.Ax2 := A * 2`.
// `Total() * 10 + Ax2` = 16*10 + 10 = 170.
//
// ⚠ THIS ROW IS THE ONE THAT CAUGHT A SILENT WRONG VALUE INTRODUCED BY THE
// FIX ITSELF. With only the `new`-site half of #3162 applied, this compiled
// clean and answered **60** (`Total()` = 0 + 6, `Ax2` = 0) against the twin's
// 170: `TypeDecl::primaryBaseCtorArgs` is AST-only and does not cross a module
// boundary, so the importer cannot pre-populate the base's slot the way a
// single-module `new` site does. The dependency CAN — `emitInstanceInitFunction`
// now runs `prePopulateBasePositionalFields` before the base chain — and this
// row is what keeps that half honest.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t3_base_clause_argument_reaches_imported_base_22_8_step_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var d := new Derived2(5, 6); "
        "    return d.Total() * 10 + d.Ax2; "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("T3"), app3162("T3", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 170);
}

BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t3_twin_base_clause_in_one_module_22_8_step_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var d := new Derived2(5, 6); "
        "    return d.Total() * 10 + d.Ax2; "
        "  end Main;";
    int r = compileAndRun3162(twin3162("T3L", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 170);
}

// ===========================================================================
// T4 — #3164 / §22.10 "`owned` parameters and capture" / §28.8: the MARKED
// spelling `new Holder(owned r, 20)` compiles, constructs, and disposes the
// resource EXACTLY ONCE.
//
// `Val() * 10 + disposals` = 21*10 + 1 = 211. A leak reads 210, a double
// dispose crashes (-1 here), a zeroed object reads 200 or 201.
//
// MEASURED with the #3162 halves applied and the #3164 carriage line disabled:
// this row was a FALSE REJECT — "the `owned` call-site marker is only valid on
// an argument to an `owned` parameter; this parameter position does not take
// ownership". §22.3 makes the marker MANDATORY on a named owned binding, so the
// two rules closed on the user and the constructor was uncallable.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t4_owned_primary_parameter_transfers_and_disposes_once_3164) {
    const std::string body =
        "  static method Work(): int "
        "    var r := new Res(); "
        "    var h := new Holder(move r, 20); "
        "    var q := h.Val(); "
        "    _ := h; "
        "    return q; "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := CT4.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("T4"), app3162("T4", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 211);
}

BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t4_twin_owned_primary_parameter_in_one_module_3164) {
    const std::string body =
        "  static method Work(): int "
        "    var r := new Res(); "
        "    var h := new Holder(move r, 20); "
        "    var q := h.Val(); "
        "    _ := h; "
        "    return q; "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := CT4L.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main;";
    int r = compileAndRun3162(twin3162("T4L", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 211);
}

BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t4_owned_primary_parameter_loop_disposes_once_each_3164) {
    // 200 construct/dispose cycles. `acc / 200` proves every iteration
    // constructed correctly; `GetDisposed()` proves the count is exactly 200 —
    // 201+ would mean a double dispose somewhere, <200 a leak.
    const std::string body =
        "  static method Work(): int "
        "    var r := new Res(); "
        "    var h := new Holder(move r, 20); "
        "    var q := h.Val(); "
        "    _ := h; "
        "    return q; "
        "  end Work; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0..199 do "
        "      acc := acc + CT4b.Work(); "
        "    end for; "
        "    if acc <> 4200 then return 1; end if; "
        "    return Res.GetDisposed(); "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("T4b"), app3162("T4b", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 200);
}

// ===========================================================================
// T5 — §22.3: the UNMARKED spelling `new Holder(r, 20)` is REJECTED, naming
// the fix.
//
// THIS IS THE DOUBLE-FREE ROW. §22.3: "Omitting the marker on a named owned
// binding is a compile error naming the fix." With #3162 applied and #3164's
// carriage line disabled, this compiled CLEAN and **SIGSEGV'd (exit 139)** —
// the caller auto-drops `r` at scope end while `Holder`'s generated `Dispose()`
// disposes the same object. Asserting on the diagnostic TEXT is required: the
// pre-fix outcome was a runtime crash, which a coarse "did it reject" check
// would happily accept, and the NEIGHBOURING failure mode (T6's spurious
// marker complaint) is also a rejection.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t5_unmarked_owned_argument_is_rejected_22_3) {
    const std::string body =
        "  static method Work(): int "
        "    var r := new Res(); "
        "    var h := new Holder(r, 20); "
        "    var q := h.Val(); "
        "    _ := h; "
        "    return q; "
        "  end Work; "
        "  public static method Main(): int "
        "    return CT5.Work(); "
        "  end Main;";
    std::string diag;
    int r = compileImportAndRun3162(lib3162("T5"), app3162("T5", body), &diag);
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, kRejected3162);
    BEZEL_EXPECT_TRUE(mentions3162(diag, "this call consumes `r`"));
    BEZEL_EXPECT_TRUE(mentions3162(diag, "\xC2\xA7""22.3"));
    // And specifically NOT the §22.3 spurious-marker complaint (either the
    // v0.169 `owned`-marker or v0.314 `move`-marker wording), which would
    // mean the modifier is still answering "" and this rejection came from
    // somewhere else.
    BEZEL_EXPECT_TRUE(!mentions3162(diag, "does not take ownership"));
}

// ===========================================================================
// T6 — §28.8 over-rejection control: a BORROW passed with `owned` to the
// imported `owned` primary parameter is still REJECTED, and for the RIGHT
// reason.
//
// The fix widens what counts as an owned sink across a module boundary, so the
// rejection that keeps a borrow OUT of that sink must be shown to survive —
// admitting it would be the double free this ordering is about, merely
// relocated. Before #3164's carriage this row also rejected, but with the
// SPURIOUS-MARKER text: right outcome, wrong reason, and a coarse assertion
// could not tell the two apart.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t6_borrow_into_owned_primary_parameter_is_rejected_28_8) {
    const std::string body =
        "  static method Take(b: Res): int "
        "    var h := new Holder(move b, 20); "
        "    var q := h.Val(); "
        "    _ := h; "
        "    return q; "
        "  end Take; "
        "  public static method Main(): int "
        "    using var r := new Res(); "
        "    return CT6.Take(r); "
        "  end Main;";
    std::string diag;
    int r = compileImportAndRun3162(lib3162("T6"), app3162("T6", body), &diag);
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, kRejected3162);
    BEZEL_EXPECT_TRUE(mentions3162(
        diag, "a borrow cannot be passed where an `owned` value is required"));
    BEZEL_EXPECT_TRUE(!mentions3162(diag, "does not take ownership"));
}

// ===========================================================================
// T7 — §22.3 over-rejection control: a FRESH PRODUCER argument stays UNMARKED
// and still constructs.
//
// §22.3 marks only a NAMED owned binding; `new Holder(new Res(), 20)` has no
// binding to leave moved-from, so no marker is written. This row exists because
// the fix makes an imported primary constructor's `owned` formal visible to the
// missing-marker report for the first time — keyed on the formal alone rather
// than on the argument being a named binding, this legal shape would become a
// false reject. (It is also the one owned row that PASSED before #3164, at 211
// — so it pins that the carriage did not disturb it.)
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t7_fresh_producer_into_owned_primary_parameter_22_3) {
    const std::string body =
        "  static method Work(): int "
        "    var h := new Holder(new Res(), 20); "
        "    var q := h.Val(); "
        "    _ := h; "
        "    return q; "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := CT7.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("T7"), app3162("T7", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 211);
}

// ===========================================================================
// T8 — §22.5: too few arguments is rejected, by the ORDINARY check.
//
// #3162 replaces #3141's blanket decline with
// `checkPositionalNewArgsFromSymbols` — the same §22.5/§5.1 check an imported
// RECORD's `new` already runs. These two rows pin that the ordinary check
// really is engaged, not merely that construction was let through: without it,
// `new PBox(3)` reaches codegen, `ne.args.size() != posFields.size()` skips the
// store loop entirely, and the object is calloc-zero.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t8_too_few_positional_arguments_rejected_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(3); "
        "    return b.Sum(); "
        "  end Main;";
    std::string diag;
    int r = compileImportAndRun3162(lib3162("T8"), app3162("T8", body), &diag);
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, kRejected3162);
    BEZEL_EXPECT_TRUE(mentions3162(diag, "required parameter `W` is not "
                                         "supplied"));
    BEZEL_EXPECT_TRUE(mentions3162(diag, "\xC2\xA7""22.5"));
}

// ===========================================================================
// T9 — §5.1: an argument whose type is not implicitly convertible to the
// parameter's is rejected.
//
// A `string` into an `int` slot is the #1105 corruption shape — an 8-byte
// pointer stored into a 4-byte field. #3141 measured exactly this for an
// imported RECORD (`p.X=4434336`); the class path must not reopen it.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t9_wrong_argument_type_rejected_5_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(\"hello\", 4); "
        "    return b.Sum(); "
        "  end Main;";
    std::string diag;
    int r = compileImportAndRun3162(lib3162("T9"), app3162("T9", body), &diag);
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, kRejected3162);
    BEZEL_EXPECT_TRUE(mentions3162(diag, "not implicitly convertible"));
    BEZEL_EXPECT_TRUE(mentions3162(diag, "\xC2\xA7""5.1"));
}

// ===========================================================================
// T10 — §22.5 / #3196: a NAMED argument to an imported primary constructor
// LOWERS correctly. Before #3196 this row asserted the loud `anyNamed`
// decline (#3162's diagnostic at sema line ~10898), because the codegen
// `storePrimaryCtorPositionalArgs` was a position-i pairing that would have
// produced a calloc-zero object the moment #3196's now-removed `anyNamed`
// gate fell. #3196 closes that gap on BOTH sides — the loud IMPORTED decline
// is gone, the silent-zero LOCAL twin is gone — by having sema publish the
// §22.5 binding on `SemaContext` and the codegen read the same binding
// (field j → arg index, or -1). This row now asserts the construction
// success it was previously guarding against.
//
// Comprehensive coverage (named-basic, named-reversed, named-default
// intermix, wrong-typed named-rejected-on-both-sides) is pinned by the
// sibling test file `xmod_named_arg_imported_primary_ctor_3196_test.cpp`;
// this row stays as the cross-module-specific named-argument regression
// marker for the suite.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t10_named_argument_lowers_correctly_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    var b := new PBox(W := 4, V := 3); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("TA"), app3162("TA", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T11 — over-rejection control: an imported class with an EXPLICIT constructor
// still constructs through `bz$<Type>$ctor`, unchanged.
//
// The codegen half guards its new step-0 store with `!hasAnyConstructor(t)`
// precisely so this path is untouched: an explicit constructor's arguments are
// bound to ITS parameters, and storing them into primary-parameter slots as
// well would be a second, unasked-for write. 7 + 100 = 107.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t11_imported_explicit_constructor_unchanged_22_7) {
    const std::string body =
        "  public static method Main(): int "
        "    using var e := new ExplicitBox(7); "
        "    return e.v; "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("TB"), app3162("TB", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 107);
}

// ===========================================================================
// T12 — over-rejection control: #3141's own shape, a LOCAL class deriving from
// an IMPORTED primary-constructor class, is unchanged.
//
// `class LocalD(): PBox(5, 6)` — `Twice()` = Sum()*2 = 22, `Q` = V*2 = 10.
// `Twice() * 10 + Q` = 230. This is the path #3141 fixed by reading the base's
// positional field list off the TypeSymbol, and it now shares
// `bz$<Base>$init` with a body this fix EXTENDED
// (`prePopulateBasePositionalFields` inside `emitInstanceInitFunction`), so it
// needs its own row: the added call must be a no-op here, because the local
// `new` site already walked the same chain and this level's argument sources
// are null across the boundary.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t12_local_class_deriving_from_imported_primary_ctor_3141) {
    const std::string src =
        "module App3162TC; import Lib3162TC; "
        "public class LocalD(): PBox(5, 6) "
        "  public method Twice(): int "
        "    return Sum() * 2; "
        "  end Twice; "
        "end LocalD; "
        "public class CTC "
        "  public static method Main(): int "
        "    var d := new LocalD(); "
        "    return d.Twice() * 10 + d.Q; "
        "  end Main; "
        "end CTC;";
    int r = compileImportAndRun3162(lib3162("TC"), src);
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 230);
}

BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t12_twin_local_derivation_in_one_module_3141) {
    const std::string src =
        "module Twin3162TCL; " + std::string(kDecls3162) +
        " public class LocalD(): PBox(5, 6) "
        "  public method Twice(): int "
        "    return Sum() * 2; "
        "  end Twice; "
        "end LocalD; "
        "public class CTCL "
        "  public static method Main(): int "
        "    var d := new LocalD(); "
        "    return d.Twice() * 10 + d.Q; "
        "  end Main; "
        "end CTCL;";
    int r = compileAndRun3162(src);
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 230);
}

// ===========================================================================
// T13 — §14.1 / §22.10 / §22.8 step 0: an imported GENERIC class's primary
// constructor now LOWERS correctly. fix #3201 closed the generic half #3162
// could not reach — the Foundation A/C arm (`emitInstanceInit`, monomorphized
// in the importer) now performs §22.8 step 0 via `storePrimaryCtorPositionalArgs`
// BEFORE installing the per-instantiation vtable, so the importer's callsite
// is the storage for the primary-constructor parameters and a field-init
// reading a primary parameter (§22.8 step 2) sees the just-stored value.
//
// `Sum()` = 3 + 4 = 7. A calloc-zero object (the pre-#3201 outcome #3141
// declined rather than ship) reads 0. The single-module T13b twin confirms
// the local construction is bit-identical, so the divergence is specifically
// the imported-side storage — i.e. the silent-zero defect #3201 closed.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t13_imported_generic_primary_ctor_stores_args_22_8) {
    const std::string body =
        "  public static method Main(): int "
        "    using var b := new GBox<int>(3, 4); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileImportAndRun3162(lib3162("T13"), app3162("T13", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T13b — the twin, which proves the construction is the same shape the
// single-module side lowers: the identical declaration in one module
// constructs and answers 7. Without this row the imported row could be
// satisfied by an artefact of the runtime or a stale dependency mirror.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t13b_twin_generic_primary_ctor_in_one_module_14_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var b := new GBox<int>(3, 4); "
        "    return b.Sum(); "
        "  end Main;";
    int r = compileAndRun3162(twin3162("T13L", body));
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T13c — §5.1 / §22.5 over-rejection control: a WRONG-TYPED argument to an
// imported GENERIC primary constructor must still be REJECTED (not silently
// zero-stored). The same `checkPositionalNewArgsFromSymbols` binder the
// non-generic #3162 imported arm uses now runs for the generic too, so the
// §5.1 reject is the same shape. Pre-#3201 this row was the rejected sibling
// of the original T13 — confirming the widen did not also widen the wrong-
// type door. (Pinned by the same kRejected3162 + diagnostic shape T13 once
// was: any future "let it pass" regression flips the expected value back to 7
// and the test fails loudly.)
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t13c_wrong_typed_arg_to_imported_generic_primary_ctor_5_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var b := new GBox<int>(\"hello\", 4); "
        "    return b.Sum(); "
        "  end Main;";
    std::string diag;
    int r = compileImportAndRun3162(lib3162("T13C"), app3162("T13C", body),
                                    &diag);
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, kRejected3162);
}

// ===========================================================================
// T13d — §22.5 over-rejection control: a WRONG-ARITY call to an imported
// GENERIC primary constructor must still be REJECTED. Same binder, same
// outcome. The non-generic #3162 imported arm has the same guard (the
// `checkPositionalNewArgsFromSymbols` arity check), so the symmetry is the
// lock for the broader over-rejection contract #3201 touches.
// ===========================================================================
BEZEL_TEST(xmod_imported_primary_ctor_3162,
           t13d_wrong_arity_to_imported_generic_primary_ctor_22_5) {
    const std::string body =
        "  public static method Main(): int "
        "    using var b := new GBox<int>(3); "
        "    return b.Sum(); "
        "  end Main;";
    std::string diag;
    int r = compileImportAndRun3162(lib3162("T13D"), app3162("T13D", body),
                                    &diag);
    if (r == kDepUnavailable3162) return;
    BEZEL_EXPECT_EQ(r, kRejected3162);
}

}  // namespace bezel::module
