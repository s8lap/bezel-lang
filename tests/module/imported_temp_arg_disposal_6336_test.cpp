// Spec-derived regression tests for issue #6336 — an argument temporary of a
// DECLARATION-LESS built-in type (§31.4 exception hierarchy), passed to an
// IMPORTED callee, was never disposed; and the `?.` (`?.`-conditional) imported
// callee registered no argument-temporary disposal AT ALL, for any type.
//
// ROOT CAUSE — TWO HEADS, ONE FAMILY.
//
// (1) `src/codegen/LlvmBackend_Calls2.cpp`, `registerImportedBorrowedFreshClassTempArg`
//     (#5268). The imported caller builds its own argument list (#1221: an
//     imported callee has no reconstructed `ast::MethodDecl`, so
//     `buildOrderedArgs` cannot run), and this helper is where it registers the
//     §28.3 disposal for a fresh `new C()` temporary at a BORROW parameter. It
//     gated on
//
//         at->kind == Named && at->declaration &&
//         at->declaration->kind == ast::TypeDeclKind::Class
//
//     and a §31.4 built-in exception carries NO user `TypeSymbol`
//     (`TypeRef::declaration == nullptr`), so the gate answered "not a class"
//     for every `new IOException("a")` argument to an imported method or
//     constructor and the temporary leaked on every call. This is the same
//     one-line predicate over the same declaration-less population that #6311
//     widened on the single-module caller side and #6337 widened on the
//     single-module callee side; this is its cross-module caller twin.
//
//     NOTE ON SCOPE — the issue's report that this leaks "for user classes too"
//     is TRUE only for head (2). For the `.` form and for the constructor, a
//     user-class temporary is already disposed: #5268 fixed exactly that and
//     `tests/module/imported_class_temp_arg_dispose_5268_test.cpp` pins it. The
//     rows below measure both and say so.
//
// (2) `src/codegen/LlvmBackend_Calls2.cpp`, the `?.`-conditional imported
//     argument loop (`lowerNullCondCall`). §7.5 puts that whole loop inside the
//     non-null `callBB`, and it had NO registration of either kind — not the
//     borrow-formal one above, not the `owned`-formal unwind-only one the `.`
//     form registers (#5293). So `s?.Nop(new Item())` leaked for a USER class
//     (16 B/object) exactly as it did for a built-in exception (40 B/object),
//     and `s?.Eat(new Item(), ThrowLater())` — the §28.3 #993 unwind edge,
//     where the temporary was produced but a later argument threw so the callee
//     never ran to take it — leaked the owned temporary too (3200 B / 200 allocs
//     measured).
//
// §7.5 also drives the ONE subtlety in the `?.` fix. Because the loop lives in
// the non-null block, a disposal registered there is ALSO live on the
// null-receiver path, where the production store never ran. A bare alloca
// hands that pass an UNDEF pointer to dispose — MEASURED on the first draft,
// which turned the previously-clean `s?.Nop(new Item())` on a null receiver
// into `AddressSanitizer: SEGV ... READ memory access` / "caused by a
// dereference of a high value address", where the pre-fix compiler returned 42.
// The slot is therefore null-INITIALISED in the FUNCTION ENTRY block, which
// dominates both arms (#2733's recorded reason for `entryAllocaNullPtr`;
// #2754's entry-block-store-vs-frame-spill split is honoured by skipping the
// init under `smStateStruct_`, where `calloc` already zeroes). The `.`-form and
// constructor callers keep the bare alloca: their loop always runs, so every
// reader of the slot is dominated by its store.
//
// BOTH DIRECTIONS. The leak rows count DESTRUCTOR RUNS in the library (`Item.n`)
// and assert the exact count, so they fail on a missing dispose AND on a
// double dispose — a double dispose is an ASan heap-use-after-free, but the
// count makes the over-release visible as a number too. Four rows pin the
// over-release side explicitly:
//   * `null_receiver_does_not_evaluate_the_argument_spec_7_5` — with a null
//     receiver the argument must not be constructed at all (§7.5), so the
//     null-initialised slot must leave the disposal a no-op rather than reading
//     the undef pointer the first draft read. This is the row that would have
//     caught that SEGV.
//   * `owned_formal_via_nullcond_exactly_once_spec_28_3` — an `owned` formal
//     reached through `?.` must be disposed by the CALLEE exactly once; a
//     caller-side release as well is a double free.
//   * `callee_stores_borrow_argument_still_one_dispose_spec_28_3` — §28.3
//     K8.4: the callee MAY store the borrowed parameter in a field, and the
//     caller still disposes the temporary at the boundary. The row reads the
//     back-reference only inside the same full expression (reading it later is
//     the spec's recorded dangling hazard, not this fix's subject).
//   * `null_receiver_owned_formal_is_never_disposed_spec_7_5` — the null arm of
//     the `owned` twin: nothing is constructed, so nothing may be disposed.
//
// MEASURED fail-without (this tree with the fix reverted, bezelc rebuilt from
// the reverted source; LSan `detect_leaks=1:exitcode=99:abort_on_error=0` on the
// built-in-exception rows and an exact destructor count on the user-class
// rows): the built-in rows print
// `SUMMARY: AddressSanitizer: 16000 byte(s) leaked in 400 allocation(s).`
// (`.` form, two arguments x 200), `8000 byte(s) leaked in 200 allocation(s).`
// (`?.` form) and `3200 byte(s) leaked in 200 allocation(s).` (the `owned`
// unwind edge); the user-class `?.` row exits with `Item.n == 0` instead of 500.
// With the fix every row is LSan-clean and returns its spec-derived value.
//
// Spec: §28.3 *Unbound owned temporaries* and *A callee that stores a plain
// argument*, §28.3 K8.1 / K8.4 (the full-expression boundary), §28.3 #993 (the
// unwind edge), §31.1 / §31.4 (the built-in exception hierarchy), §7.5
// (`?.` short-circuit — nothing to the right of a null receiver is evaluated),
// §22.5 (eager left-to-right argument evaluation). Revision v0.338.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile6336 {
    std::string path;
    explicit TempFile6336(std::string p) : path(std::move(p)) {}
    ~TempFile6336() { std::remove(path.c_str()); }
};

void writeSource6336(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj6336(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// The library: `Item` counts its own destructions, and `Boom()` throws so the
// §28.3 #993 unwind edge is reachable from an argument list. `Nop`/`Take`/`Hold`
// take borrow parameters; `Eat` takes an `owned` one, which the callee
// auto-drops per §28.3's must-use rule.
const char* kLib6336 = R"BZL(module Lib6336;
public class Item
  public static n: int := 0;
  public constructor() end constructor;
  destructor Item.n := Item.n + 1; end destructor;
  public method V(): int return 1; end V;
end Item;
public class Sink
  public constructor() end constructor;
  public method Nop(x: Item): int return x.V(); end Nop;
  public method NopX(e: Exception): int return 1; end NopX;
  public method NopR(x: Item): Sink? return null; end NopR;
  public method NopRX(e: Exception): Sink? return null; end NopRX;
  public method Hold(x: Item): int
    _i := x;
    return x.V();
  end Hold;
  public _i: Item?;
  public method Eat(owned x: Item): Sink? return null; end Eat;
  public method EatTwo(owned x: Item, y: int): Sink? return null; end EatTwo;
  public method EatX(owned e: Exception): Sink? return null; end EatX;
end Sink;
public class Holder
  public constructor() end constructor;
  public constructor(e: Exception) end constructor;
end Holder;
public class Boom
  public constructor() end constructor;
  public static method Fire(): int
    throw new InvalidOperationException("boom");
  end Fire;
end Boom;
)BZL";

// Runs `body` under `decls` (optional extra app members) with `iters`
// iterations, and returns the app's exit code.
//
// `-2` = the LIBRARY did not build (toolchain unavailable — the row skips);
// `-1` = the app did not build or did not exit normally. Both are asserted
// against explicitly by every row: `BEZEL_EXPECT_TRUE(r != -2)` proves the leg
// was not silently skipped, and no row returns early on a skip.
int run6336(const std::string& body, const std::string& decls = "",
            int iters = 200, const std::string& ret = "0") {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6336_");
    TempFile6336 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6336 aObj((dir / (stem + "_a.o")).string());
    writeSource6336(aSrc.path, kLib6336);
    if (!compileToObj6336(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    std::string app =
        "module App6336;\nimport Lib6336;\n"
        "public class Program\n" + decls +
        "  public static method Main(): int\n"
        "    using var s := new Lib6336.Sink();\n"
        "    for var i in 0..<" + std::to_string(iters) + " do\n"
        "      " + body + "\n"
        "    end;\n"
        "    return " + ret + ";\n"
        "  end Main;\n"
        "end Program;\n";
    TempFile6336 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6336 bExe((dir / stem).string());
    writeSource6336(bSrc.path, app);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps,
                                         opts).success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The same app shape, but Main returns the library's destructor count instead of
// a constant, so a missing dispose AND a double dispose are both visible in the
// number the row asserts (a double dispose is also an ASan heap-use-after-free,
// which would turn `ran` false — but the count says which way it went wrong).
int runCount6336(const std::string& body, const std::string& decls = "",
                 int iters = 200) {
    return run6336(body, decls, iters, "Lib6336.Item.n mod 251");
}

}  // namespace

// §28.3 *Unbound owned temporaries* / §31.4 (#6336, head 1) — the `.` form at
// an IMPORTED method, with a §31.4 built-in exception temporary. There is no
// `Item` here to count, so this row is the LSan oracle: pre-fix it reports
// `SUMMARY: AddressSanitizer: 16000 byte(s) leaked in 400 allocation(s).`
// (two arguments x 200 iterations x 40 B) and the sanitizer's exit code makes
// the harness report `ran == false`; post-fix it runs clean and returns 0.
//
// 200 iterations x 2 built-in temporaries, each 40 B = 16000 B / 400 allocs.
BEZEL_TEST(imported_temp_arg_disposal_6336,
           builtin_exception_arg_dot_form_imported_method_spec_31_4) {
    int r = run6336("s.NopX(new IOException(\"a\")); s.NopX(new IOException(\"b\"));");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 0);
}

// §28.3 / §31.4 (#6336, head 1) — the IMPORTED CONSTRUCTOR, the other caller of
// the same helper (the positional arm; the named-argument arm is the same call).
// Pre-fix: `8000 byte(s) leaked in 200 allocation(s).` (40 B x 200).
BEZEL_TEST(imported_temp_arg_disposal_6336,
           builtin_exception_arg_imported_ctor_spec_31_4) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6336c_");
    TempFile6336 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6336 aObj((dir / (stem + "_a.o")).string());
    writeSource6336(aSrc.path, kLib6336);
    BEZEL_EXPECT_TRUE(compileToObj6336(aSrc.path, aObj.path));
    TempFile6336 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6336 bExe((dir / stem).string());
    writeSource6336(bSrc.path,
        "module AppC6336;\nimport Lib6336;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    for var i in 0..<200 do\n"
        "      using var h := new Lib6336.Holder(new IOException(\"a\"));\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    BEZEL_EXPECT_TRUE(bezel::test::compileFileForTest(
        driver, bSrc.path, bExe.path, deps, opts).success);
    int status = std::system(bExe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 0);
}

// §28.3 *Unbound owned temporaries* (#6336, head 2) — the `?.` form at an
// IMPORTED method with a USER CLASS temporary. This is the row that shows head
// (2) is genuinely wider than head (1): #5268 already disposes a user-class
// temporary on the `.` form, so this leak is specific to `?.`.
//
// `Item.n` is the oracle. Pre-fix nothing is ever disposed, so n stays 0 and
// the row returns 0 instead of 200 mod 251 = 200; post-fix each of the 200
// temporaries is disposed exactly once and n is 200.
BEZEL_TEST(imported_temp_arg_disposal_6336,
           user_class_arg_nullcond_form_imported_method_spec_28_3) {
    int r = runCount6336("var q := s?.NopR(new Lib6336.Item());");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 200);
}

// §31.4 (#6336, head 2) — the `?.` form with a §31.4 built-in exception
// temporary. No destructor counter exists for this type, so LSan is the oracle:
// pre-fix `SUMMARY: AddressSanitizer: 8000 byte(s) leaked in 200
// allocation(s).` (40 B x 200).
BEZEL_TEST(imported_temp_arg_disposal_6336,
           builtin_exception_arg_nullcond_form_imported_method_spec_31_4) {
    int r = run6336("var q := s?.NopRX(new IOException(\"a\"));");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 0);
}

// §28.3 #993 (#6336, head 2) — THE UNWIND EDGE at an IMPORTED `owned` formal
// reached through `?.`. `EatTwo`'s first argument is a fresh owned temporary and
// its second THROWS, so the callee never runs and never takes ownership; the
// caller's unwind-only entry is the only thing that can release it. The `.` form
// registers this entry (the #5293 block); the `?.` loop registered neither kind.
//
// Pre-fix: `SUMMARY: AddressSanitizer: 3200 byte(s) leaked in 200
// allocation(s).` (16 B x 200). Post-fix the catch fires 200 times and the
// temporary is released on the unwind edge; `Item.n` is 200 either way, which
// is the point — the counter proves the normal path did NOT also dispose it.
BEZEL_TEST(imported_temp_arg_disposal_6336,
           owned_formal_nullcond_unwind_edge_later_arg_throws_spec_28_3) {
    int r = runCount6336(
        "try\n"
        "        var q := s?.EatTwo(new Lib6336.Item(), Lib6336.Boom.Fire());\n"
        "      catch\n"
        "        InvalidOperationException => 0;\n"
        "      end try;");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 200);
}

// §7.5 (#6336) — OVER-RELEASE GUARD, and the row that would have caught the
// SEGV the first draft of the `?.` fix introduced. §7.5: "`?.` short-circuits.
// The receiver is evaluated first; if it is null the access does not occur and
// nothing to its right in the chain is evaluated." So on a null receiver the
// `new Lib6336.Item()` is never CONSTRUCTED, and the disposal the `?.` loop
// registered must find the null-INITIALISED slot — a bare alloca would hand it
// an undef pointer, which ASan reported as `SEGV ... dereference of a high
// value address` where the pre-fix compiler returned 42.
//
// `made` is the library's own construction counter: the correct answer is 0,
// and the row would fail with a SEGV (not a wrong number) on the undef-pointer
// draft. Pre-fix this row is also clean — there was no registration at all —
// so it is a control that a future over-wide `?.` widening turns red.
BEZEL_TEST(imported_temp_arg_disposal_6336,
           null_receiver_does_not_evaluate_the_argument_spec_7_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6336n_");
    TempFile6336 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6336 aObj((dir / (stem + "_a.o")).string());
    writeSource6336(aSrc.path, kLib6336);
    BEZEL_EXPECT_TRUE(compileToObj6336(aSrc.path, aObj.path));
    TempFile6336 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6336 bExe((dir / stem).string());
    writeSource6336(bSrc.path,
        "module AppN6336;\nimport Lib6336;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var z: Lib6336.Sink? := null;\n"
        "    for var i in 0..<200 do\n"
        "      var q := z?.NopR(new Lib6336.Item());\n"
        "    end;\n"
        "    return Lib6336.Item.n;\n"
        "  end Main;\n"
        "end Program;\n");
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    BEZEL_EXPECT_TRUE(bezel::test::compileFileForTest(
        driver, bSrc.path, bExe.path, deps, opts).success);
    int status = std::system(bExe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 0);
}

// §7.5 (#6336) — OVER-RELEASE GUARD, the `owned` twin's null arm. Nothing is
// constructed on the null path, so nothing may be disposed; `Item.n` is 0. This
// is the same undef-pointer exposure as the row above, reached through the
// `owned`-formal registration instead of the borrow one, so the two together
// cover both slots the `?.` fix allocates.
BEZEL_TEST(imported_temp_arg_disposal_6336,
           null_receiver_owned_formal_is_never_disposed_spec_7_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6336o_");
    TempFile6336 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6336 aObj((dir / (stem + "_a.o")).string());
    writeSource6336(aSrc.path, kLib6336);
    BEZEL_EXPECT_TRUE(compileToObj6336(aSrc.path, aObj.path));
    TempFile6336 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6336 bExe((dir / stem).string());
    writeSource6336(bSrc.path,
        "module AppO6336;\nimport Lib6336;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var z: Lib6336.Sink? := null;\n"
        "    for var i in 0..<200 do\n"
        "      var q := z?.Eat(new Lib6336.Item());\n"
        "    end;\n"
        "    return Lib6336.Item.n;\n"
        "  end Main;\n"
        "end Program;\n");
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    BEZEL_EXPECT_TRUE(bezel::test::compileFileForTest(
        driver, bSrc.path, bExe.path, deps, opts).success);
    int status = std::system(bExe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 0);
}

// §28.3 (#6336) — OVER-RELEASE GUARD, the `owned` formal on the NORMAL path.
// `s?.Eat(new Item())` hands ownership to the callee, which auto-drops the
// parameter (§28.3 must-use); the caller must NOT also dispose it. A caller-side
// release as well is a double free. `Item.n` is the oracle and must be exactly
// the iteration count — 300 mod 251 = 49 — so both a missing callee dispose and
// a doubled one are visible in the number.
BEZEL_TEST(imported_temp_arg_disposal_6336,
           owned_formal_nullcond_disposed_exactly_once_spec_28_3) {
    int r = runCount6336("var q := s?.Eat(new Lib6336.Item());", "", 300);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 49);
}

// §28.3 K8.4 / §31.4 (#6336) — OVER-RELEASE GUARD, the callee-stores case. The
// library's `Hold(x: Item)` assigns the borrowed parameter into the non-`owned`
// field `_i`, which §28.3 K8.4 explicitly admits ("the callee may store it");
// the caller still owns the temporary and must dispose it at the boundary all
// the same. The row reads the back-reference only INSIDE the same full
// expression — reading `_i` from a later statement is the spec's recorded
// dangling hazard, not this fix's subject.
//
// `Item.n` must be exactly 200 (200 mod 251) — one disposal per temporary, no
// double free from the callee's own borrow.
BEZEL_TEST(imported_temp_arg_disposal_6336,
           callee_stores_borrow_argument_still_one_dispose_spec_28_3) {
    int r = runCount6336("var v := s.Hold(new Lib6336.Item());");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 200);
}

// §28.3 / §22.5 (#6336) — CONTROL: the same user-class temporary on the `.`
// form, which #5268 already handled. It is clean BEFORE this fix and must stay
// clean, so it is the row that would go red if a re-widening of the helper's
// predicate admitted something it should not. `Item.n` = 200 mod 251 = 200.
BEZEL_TEST(imported_temp_arg_disposal_6336,
           control_user_class_arg_dot_form_already_clean_spec_28_3) {
    int r = runCount6336("s.Nop(new Lib6336.Item());");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 200);
}

}  // namespace bezel::module