// Codegen — issue #5313: `Channel<int>` returned by an IMPORTED factory sends
// nothing, `IsClosed` emits nothing, and the channel leaks (#2992) — stdlib
// §6.9 / §30.15, v0.313.
//
// THE SPEC. stdlib §6.9 / spec §30.15 (v0.313): `Channel<T>.Send` (on a
// `ChannelSender<T>` obtained from `NewSender()`) followed by
// `TryReceive(out item)` delivers the item, and a `using var` binding of the
// channel (and of the sender) disposes both at scope end.
//
// THE DEFECT — a THIRD TypeRef shape the hand-rolled predicates reject.
//
// A Named `TypeRef` reaches codegen in one of three shapes (see
// `Emitter::builtinTypeName`, src/codegen/LlvmBackend_Calls.cpp:801, the
// chokepoint #2809/#2997/#662 established):
//
//   a) declaration-less with `astNode->name` — a LOCAL written occurrence;
//   b) `declaration` = a synthetic `builtinClassSym`;
//   c) declaration-less with `astNode == nullptr` and the name in
//      `importedName` — an IMPORTED builtin.
//
// Shape (c) exists because `ModuleLoader::convert()`
// (src/module/ModuleLoader.cpp:344-385) builds the TypeRef for a name read
// out of a dependency's `.bezel.meta`; there is no AST for another module's
// source, so `astNode` is null. `lookupNamed` returns null exactly when the
// named type is NOT an exported user type of the dependency, and that is
// when the loader stamps `importedName` (the '@'-spelled §2.2
// alias-qualified built-in spelling takes the same carriage, gated there on
// `builtinTypeHomeModuleOf(bare) == <home module>`).
//
// FOUR sites each hand-rolled shape (a)'s PRECONDITION —
// `!rt->declaration && rt->astNode && rt->astNode->name && !parts.empty()`
// — and then read `rt->headName()`. But `headName()`
// (src/sema/TypeRef.hpp:80) ALREADY falls back to `importedName`. The
// precondition, not the name read, is what rejected shape (c): each site
// returned early on the null `astNode` and never reached a head it would
// have resolved correctly. `builtinTypeName` never had the bug because it
// tests the shapes in order and has an explicit `importedName` branch.
//
// MEASURED REPRO (issue's own — two-module build, BEZEL_DUMP_IR):
//
//   pre-fix  `Send(5)`   → no `bz_channel_sender_send` call emitted at all
//           `IsClosed`   → no `bz_channel_is_closed` call emitted at all;
//                           #3079 "the argument of this Console.WriteLine
//                           failed to lower to a value, so the whole
//                           statement emits nothing — not even the line
//                           terminator"
//           `using var`  → no `bz_channel_dispose` /
//                           `bz_channel_sender_release`; #2992
//                           "`using var` of disposable built-in `Channel` has
//                           no disposal arm — the value leaks at scope end"
//           program prints `False` and then ONE line (two WriteLine calls,
//                           one line of output), exit 0 — SILENT WRONG.
//   post-fix prints `True` / `False`, delivers payload `5`, and both
//           disposal calls are present in the IR.
//
// The per-site consequence, in the order the four predicates are reached:
//
//   1. sema `builtinInstName` (ExprTypecheck_Calls.cpp:11839) gates every
//      `rcn == "…"` arm. Unfixed, the `Channel<T>` arm never ran, so
//      `c2.NewSender()` was left UNTYPED — the local `t2` had no TypeRef at
//      all, which is why `t2.Send(5)` found no receiver type and emitted
//      nothing. This is the deepest site: it starves the other three.
//   2. codegen `isChProp` (LlvmBackend_Exprs.cpp:4594) is the `IsClosed` /
//      `Count` PROPERTY-read arm — a MemberAccessExpr without parens, which
//      bypasses `tryLowerChannelAndDomainCall` entirely, so it needs its own
//      read rather than inheriting the call arm's.
//   3. + 4. the `using var` `Channel` / `ChannelSender` disposal arms
//      (LlvmBackend_Setup.cpp:21047, :21074) and the §28.3 auto-drop sibling
//      (:22678) — these decide whether a `d.channel` / `d.channelSender` /
//      the type-specific runtime release is registered at all. Skipped, the
//      graph leaks and the channel stays open (it was the last sender).
//
// FIX. All four read the head through `builtinTypeName`, the existing
// chokepoint that already sees all three shapes. No new predicate is
// introduced, and no test is weakened.
//
// SOUNDNESS / BLAST RADIUS. The widening can only ADD a match where the
// hand-rolled precondition used to reject: a declaration-less Named TypeRef
// with a null `astNode`. `importedName` is populated only on a name that
// `lookupNamed` did not resolve to an exported user type, or on a
// `builtinTypeHomeModuleOf`-verified built-in spelling — so a match is still
// a compiler built-in, never a user class that merely shares a name
// (`Emitter::builtinTypeName`'s #2997 `moduleName` discriminator continues to
// reject an IMPORTED USER class, which arrives with a non-null `declaration`).
// A local occurrence is unchanged: it was already shape (a) and already
// matched.
//
// ── ORACLE ────────────────────────────────────────────────────────────────
//
// Two-module AOT (`Driver::compileObject` for the library, then
// `Driver::compileFile` for the executable, both in-process, each forked —
// the strict AOT oracle). The load-bearing oracle is STDOUT: the issue's
// expected program prints `True` then `False`, and the 200-iteration loop
// prints the payload `5` on every pass. Pre-fix the same programs print
// `False` and one line short (and `none` on every loop pass), so stdout
// alone separates the two — no diagnostic-parsing needed. The ASan leg
// (`sanitizeAddress = true`) is the LEAK oracle for the two disposal sites,
// over 200 iterations so a per-iteration leak cannot hide.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base (DISK-backed
// per CLAUDE.md). The library object MUST outlive the app build — a removed
// .o shows up as `bezelc: error: cannot load module metadata from ...`. The
// `TempFile` shared_ptrs in the helpers below keep it open.
static const std::filesystem::path& wtDir5313() {
    return wtScratchDir("wt-5313");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream(path) << src;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::filesystem::path& dir,
                   const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_5313lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
    bool ok = false;
};

AppHandle buildApp(const std::filesystem::path& dir,
                   const std::string& libObjPath,
                   const std::string& appSrc,
                   bool sanitize) {
    AppHandle h;
    std::string stem = unique_stem("bezel_5313app_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    h.exe = std::make_shared<TempFile>((dir / stem).string());
    writeSource(h.src->path, appSrc);
    bezel::Driver driver;
    // An empty `libObjPath` is the SAME-MODULE control, which has no
    // dependency: it must still compile and link. Only a caller that
    // genuinely failed to build its library passes "" — and `buildLib` is
    // always checked with `BEZEL_EXPECT_TRUE` before this runs.
    std::vector<bezel::Dependency> deps;
    if (!libObjPath.empty()) {
        bezel::Dependency d;
        d.metadataPath = libObjPath;
        d.objectPath = libObjPath;
        deps.push_back(d);
    }
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = sanitize;
    bezel::CompileResult rb = driver.compileFile(h.src->path, h.exe->path,
                                                  deps, opts);
    h.ok = rb.success;
    h.exePath = h.exe->path;
    return h;
}

// popen and capture {exitCode, combined-stdout-and-stderr}.
struct RunResult { int code; std::string out; };
RunResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// The issue's exact `lib.bzl`: a PUBLIC class whose static factory returns a
// `Channel<int>`. The factory is what puts the channel in shape (c) — the
// return type is read out of the dependency's metadata, not from an AST.
const std::string kLibSrc =
    "module Lib; "
    "public class Hub "
    "  public static method Make(): Channel<int> "
    "    return new Channel<int>(); "
    "  end Make; "
    "end Hub; ";

// The issue's exact `app.bzl`. `TryReceive` must report `True` after
// `Send(5)`, and `IsClosed` must print its own line.
const std::string kAppSrc =
    "module App; "
    "import Lib; "
    "class C "
    "  public static method Main(): int "
    "    using var c2 := Hub.Make(); "
    "    using var t2 := c2.NewSender(); "
    "    t2.Send(5); "
    "    var v: int? := null; "
    "    Console.WriteLine(c2.TryReceive(out v)); "
    "    Console.WriteLine(c2.IsClosed); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// The same two modules with the factory in the SAME module — the issue's
// control. This program is CORRECT pre-fix (`True` / `False`), so it pins
// that the fix does not change the local path and that the test's oracle
// discriminates on the import, not on the program text.
const std::string kLocalAppSrc =
    "module App; "
    "class Hub "
    "  public static method Make(): Channel<int> "
    "    return new Channel<int>(); "
    "  end Make; "
    "end Hub; "
    "class C "
    "  public static method Main(): int "
    "    using var c2 := Hub.Make(); "
    "    using var t2 := c2.NewSender(); "
    "    t2.Send(5); "
    "    var v: int? := null; "
    "    Console.WriteLine(c2.TryReceive(out v)); "
    "    Console.WriteLine(c2.IsClosed); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// 200 iterations, one `using var` channel + sender pair each. The payload
// line is the load-bearing oracle: pre-fix `Send` emits nothing so every
// pass takes the `else` arm and prints `none`; post-fix every pass prints
// `5`. 200 is the issue's own count and also the LEAK multiplier for the
// two disposal sites (a per-iteration leak cannot hide at that count).
const std::string kLoopAppSrc =
    "module App; "
    "import Lib; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 200 do "
    "      using var c2 := Hub.Make(); "
    "      using var t2 := c2.NewSender(); "
    "      t2.Send(5); "
    "      var v: int? := null; "
    "      if c2.TryReceive(out v) then "
    "        Console.WriteLine($\"{v}\"); "
    "      else "
    "        Console.WriteLine(\"none\"); "
    "      end if; "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// Count how many times `needle` occurs in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

}  // namespace

// ── HEADLINE — issue's exact repro, plain build ──────────────────────────
//
// Pre-fix: stdout is `False\n` — ONE line for two `Console.WriteLine` calls,
// exit 0. The `IsClosed` line is absent because that statement emitted
// nothing at all, not even its terminator (#3079). Post-fix: `True\nFalse\n`.
BEZEL_TEST(imported_channel_factory_send_receive_5313,
           issue_repro_send_then_tryreceive_delivers) {
    auto dir = wtDir5313();
    LibHandle libH = buildLib(dir, kLibSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kAppSrc, /*sanitize=*/false);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    // The issue reports exit 0 pre-fix too — the defect is SILENT, so exit
    // code is not the oracle. STDOUT is.
    BEZEL_EXPECT_EQ(rr.code, 0);
    BEZEL_EXPECT_EQ(rr.out, std::string("True\nFalse\n"));
}

// ── CONTROL — same program with the factory in the SAME module ───────────
//
// MEASURED on r23-c28 BEFORE the fix: this already prints `True\nFalse\n`.
// It is the issue's own control, and it is what makes the headline row
// non-vacuous — the two programs differ only in whether `Hub.Make` is
// imported, so a pass on the headline row is attributable to the import
// path, not to the program text. This row passes both before and after the
// fix by design; it is here to catch a regression in the local path, and it
// is NOT counted as a FAIL-WITHOUT discriminator.
BEZEL_TEST(imported_channel_factory_send_receive_5313,
           control_same_module_factory_already_worked) {
    auto dir = wtDir5313();
    AppHandle appH = buildApp(dir, std::string(), kLocalAppSrc,
                              /*sanitize=*/false);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(rr.code, 0);
    BEZEL_EXPECT_EQ(rr.out, std::string("True\nFalse\n"));
}

// ── PAYLOAD — the item itself, not just the boolean ──────────────────────
//
// A `True` from `TryReceive` with a wrong payload would satisfy the
// headline row. This row prints the received value, so it pins the decode
// end too: pre-fix `Send` emits nothing and the program prints `False`; the
// `none` line is unreachable because `TryReceive` returns false.
BEZEL_TEST(imported_channel_factory_send_receive_5313,
           payload_value_arrives_intact) {
    auto dir = wtDir5313();
    LibHandle libH = buildLib(dir, kLibSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    const std::string src =
        "module App; "
        "import Lib; "
        "class C "
        "  public static method Main(): int "
        "    using var c2 := Hub.Make(); "
        "    using var t2 := c2.NewSender(); "
        "    t2.Send(5); "
        "    var v: int? := null; "
        "    Console.WriteLine(c2.TryReceive(out v)); "
        "    Console.WriteLine($\"{v}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    AppHandle appH = buildApp(dir, libH.objPath, src, /*sanitize=*/false);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(rr.code, 0);
    BEZEL_EXPECT_EQ(rr.out, std::string("True\n5\n"));
}

// ── LEAK — the two disposal arms, 200 iterations ────────────────────────
//
// Pre-fix the `using var` Channel / ChannelSender arms are skipped, so no
// `d.channel` / `d.channelSender` is registered, and the #2992 end-of-chain
// guard fires the `internal:` warning. Post-fix both are registered and
// 200 pairs are released, so LSan reports 0. The payload count (200 x `5`)
// is asserted alongside: a build that disposed correctly but stopped
// delivering would still fail this row.
BEZEL_TEST(imported_channel_factory_send_receive_5313,
           disposal_arms_registered_no_leak_200_iters) {
    auto dir = wtDir5313();
    LibHandle libH = buildLib(dir, kLibSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kLoopAppSrc,
                              /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(rr.code, 0);
    // Every iteration delivered the payload (never `none`).
    BEZEL_EXPECT_EQ(countOccurrences(rr.out, "5\n"), 200);
    BEZEL_EXPECT_EQ(countOccurrences(rr.out, "none"), 0);
    // LSan: the disposal arms ran, so nothing leaked. Pre-fix this program
    // leaks 1400 objects / 411 KB (the issue's measurement) and prints
    // `none` 200 times.
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("leaked") == std::string::npos);
}

// ── COUNT PROPERTY — the sibling of the `IsClosed` property arm ─────────
//
// `IsClosed` and `Count` are lowered by the SAME hand-rolled predicate
// (LlvmBackend_Exprs.cpp:4594 gates both names), so fixing only `IsClosed`
// would leave `Count` broken. This row pins the sibling. Pre-fix the
// `Console.WriteLine(c2.Count)` statement emits nothing (#3079), so stdout
// is empty.
BEZEL_TEST(imported_channel_factory_send_receive_5313,
           count_property_read_lowers) {
    auto dir = wtDir5313();
    LibHandle libH = buildLib(dir, kLibSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    const std::string src =
        "module App; "
        "import Lib; "
        "class C "
        "  public static method Main(): int "
        "    using var c2 := Hub.Make(); "
        "    using var t2 := c2.NewSender(); "
        "    t2.Send(5); "
        "    Console.WriteLine(c2.Count); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    AppHandle appH = buildApp(dir, libH.objPath, src, /*sanitize=*/false);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(rr.code, 0);
    // One item was sent and not yet received, so Count is 1.
    BEZEL_EXPECT_EQ(rr.out, std::string("1\n"));
}

}  // namespace bezel::codegen
