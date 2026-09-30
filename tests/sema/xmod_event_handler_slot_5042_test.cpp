// Regression for #5042 (event half) — an IMPORTED `event` member's handler
// slot must be classified exactly like its local twin at a direct
// `Subscribe(h)` (stdlib §6.5 / spec §24 / §27.1, v0.313).
//
// #4968 added the handler-slot check (`plainEventHandlerSlot`) but read the
// event's payload / `async` marker off `EventSymbol::decl` only. An imported
// event has no decl, so the check ran with asyncKnown = false and SKIPPED the
// `(T) -> Task` test: MEASURED on origin/main d1827572 (lib
// `public async event ATick: int;`, app `b.ATick.Subscribe(P.Sync)` with a
// unit-returning handler) the app compiled clean, while the same shape on a
// local `async event` is rejected. The fix reads the slot through the
// both-sources accessors `eventPayloadResolvedType` / `eventHandlerIsAsync`
// (the wire bit MetaMember::isEventAsync, #2421).
//
// Row 1 FAILS WITHOUT the fix (async import, sync handler now rejected).
// Rows 2-4 are pins that pass on both sides: a matching async handler is
// accepted; the sync event still rejects a wrong-arity handler and accepts a
// matching one (the payload path was already correct for imports).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::sema {
namespace {

struct TempFile5042 {
    std::string path;
    explicit TempFile5042(const std::string& p) : path(p) {}
    ~TempFile5042() { std::remove(path.c_str()); }
};

void writeSource5042(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

const std::string kLib5042 =
    "module XLib5042;\n"
    "public class Btn\n"
    "  public event Tick: int;\n"
    "  public async event ATick: int;\n"
    "end Btn;\n";

// Compiles the lib to an object, then the app against it; true iff the APP
// compiled.
bool compileApp5042(const std::string& appBody) {
    auto dir = wtScratchDir("wt-5042");
    std::string stem = unique_stem("bezel_5042_");
    TempFile5042 lsrc((dir / (stem + "_lib.bzl")).string());
    TempFile5042 lobj((dir / (stem + "_lib.o")).string());
    writeSource5042(lsrc.path, kLib5042);
    bezel::Driver ld;
    if (!ld.compileObject(lsrc.path, lobj.path).success) return false;
    TempFile5042 asrc((dir / (stem + "_app.bzl")).string());
    TempFile5042 aexe((dir / (stem + "_app")).string());
    writeSource5042(asrc.path,
        "module XMain5042; import XLib5042;\n"
        "class P\n"
        "  public static method Sync(a: int) end Sync;\n"
        "  public static method Two(a: int, b: int) end Two;\n"
        "  public static async method AsyncH(a: int): Task "
        "end AsyncH;\n"
        "  public static method Main(): int\n"
        "    using var b := new Btn();\n" + appBody +
        "    return 0;\n  end Main;\nend P;\n");
    bezel::Driver ad;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lobj.path;
    d.objectPath = lobj.path;
    deps.push_back(d);
    return ad.compileFile(asrc.path, aexe.path, deps).success;
}

}  // namespace

// FAILS WITHOUT THE FIX: `(int) -> ()` handler on an imported `async event`.
BEZEL_TEST(xmod_event_handler_slot_5042,
           imported_async_event_sync_handler_rejected_stdlib_6_5) {
    BEZEL_EXPECT_FALSE(compileApp5042(
        "    using var s := b.ATick.Subscribe(P.Sync);\n"));
}

BEZEL_TEST(xmod_event_handler_slot_5042,
           imported_async_event_task_handler_accepted_stdlib_6_5) {
    BEZEL_EXPECT_TRUE(compileApp5042(
        "    using var s := b.ATick.Subscribe(P.AsyncH);\n"));
}

BEZEL_TEST(xmod_event_handler_slot_5042,
           imported_event_wrong_arity_handler_rejected_stdlib_6_5) {
    BEZEL_EXPECT_FALSE(compileApp5042(
        "    using var s := b.Tick.Subscribe(P.Two);\n"));
}

BEZEL_TEST(xmod_event_handler_slot_5042,
           imported_event_matching_handler_accepted_stdlib_6_5) {
    BEZEL_EXPECT_TRUE(compileApp5042(
        "    using var s := b.Tick.Subscribe(P.Sync);\n"));
}

}  // namespace bezel::sema
