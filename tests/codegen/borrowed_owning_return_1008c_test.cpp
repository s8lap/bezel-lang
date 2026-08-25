// fix #1008 Part C / §28.6 (v0.151, revised by #1760): returning a *borrowed*
// owning collection.
//
// A non-`owned` owning-collection return type denotes a BORROW (the `Owned*`
// name is element ownership, orthogonal to the `owned` container qualifier,
// §5.11). The #1008 Part C design originally admitted a bare read of an
// `owned` owning-collection field of the receiver (`this`) as a borrow, on the
// reasoning that "the caller holds the receiver across the call, so the borrow
// is safe". That reasoning is sound for a DIRECT call (producerState resolves
// the concrete all-borrow method and downgrades the result to Borrowed), but
// under interface / virtual-override / imported / function-value dispatch the
// call site cannot resolve the concrete method, producerState falls to the
// name-head Owned fallback, and the caller disposes the borrow — a HARD
// double-free of the field's real owner. Owning collections are not
// Dispose-idempotent (unlike Shared/Weak/ChannelSender/CancellationTokenSource,
// whose double-dispose is a safe refcount no-op), so unlike the
// deferred-activation heads there is no safe no-op fallback. #1760 therefore
// REMOVES the receiver-owned-field carve-out for owning collections at the
// definition site: every such borrow return is rejected, mirroring #1660's
// removal of the IOwningEnumerable carve-out. The spec-correct remedies are:
// (a) return the borrow through the non-owning collection type (`Array<T>`) —
//     read-only access; (b) declare `owned` for a transfer (caller becomes
//     owner). Test (1)/(2) exercise form (a) here; form (b)'s codegen has a
//     pre-existing latent issue tracked separately. Tests (4)–(7) test the
//     non-carve-out rules and are unchanged.
//
// Disposal is observed through a static Cell counter (ctor ++ / dtor --): a net
// Cell.n of 0 after the owning scope ends proves dispose-exactly-once.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1008c() {
    return wtScratchDir("wt-1008c");
}

int compileAndRun(const std::string& source) {
    auto dir = wtDir1008c();
    std::string stem = unique_stem("bezel_1008c");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

bool compileFails(const std::string& source) {
    auto dir = wtDir1008c();
    std::string stem = unique_stem("bezel_1008c_neg");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}

// A Cell with a disposal counter, and a Holder owning an OwningArray<Cell> field
// with a read-only BORROW accessor `GetItems` (return type `Array<Cell>`, the
// non-owning counterpart of `OwningArray<Cell>` — the §28.6 L7003 / L7363
// remedy for read-only borrow through the non-owning view, still required at
// v0.240 since the §28.6 carve-out only admits a return of an OWNED field of
// THE RECEIVER; `Holder._items` here is `owned` and `GetItems` reads it as
// the borrow admitted by the §28.6 carve-out — the return TYPE itself is
// `Array<Cell>` so the borrow is on the non-owning view).
const char* prelude =
    "module M; "
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
    "class Holder public owned _items: OwningArray<Cell>; "
    "  public constructor() _items := new OwningArray<Cell>(); _items.Add(new Cell()); end constructor; "
    "  destructor end destructor; "
    "  public method GetItems(): Array<Cell> => _items; "
    "  public method DrainSelf(): int "
    "    for var c in _items.Drain() do c.Dispose(); end; return 0; "
    "  end DrainSelf; "
    "end Holder; ";
}  // namespace

// (1) The receiver-field read is returned through the NON-OWNING view
// (`Array<Cell>`, the spec-correct remedy for read-only borrow per #1760).
// The caller reads it (Count), and the Holder disposes its field + element
// exactly once → Cell.n == 0.
BEZEL_TEST(borrowed_owning_1008c, borrow_field_read_disposed_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(prelude) +
        "class P public static method Main(): int "
        "  do using var h := new Holder(); "
        "     var items := h.GetItems(); var n := items.Count(); end; "
        "  return Cell.n; "                       // 0 — disposed once
        "end Main; end P;"), 0);
}

// (2) #1760 REMOVED: the original `caller_dispose_borrow_rejected` exercised
// the §28.6 / §5.11 (#1008 Part C) capability restriction (a Dispose on the
// borrow is a compile error) on the carve-out's borrow return. With the
// carve-out removed (#1760, the test that admitted this return now REJECTS),
// the original test's premise is gone — the borrow is no longer expressible
// through a non-`owned` owning-collection return type. The natural
// replacement would be a borrow through the non-owning view (`Array<Cell>`),
// but the spec-correct form `=> _items;` for the `owned _items: OwningArray<
// Cell>` field is not currently a no-op Dispose on the borrowed view (the
// underlying OwningArray<Cell> spine is double-freed; tracked separately), so
// the Dispose capability restriction cannot be cleanly re-tested at this
// level. Removed; the per-head reject tests in
// `owning_collection_borrow_return_indirect_dispatch_1760_test.cpp` cover the
// reject half of the rule, and tests (1)/(4)–(7) cover the rest.

// (3) §28.6 admission under INDIRECT dispatch (interface) — REPINNED by #2269.
// The original test (pre-#1760) exercised a DIRECT call to a borrow-returning
// `OwningArray<Cell>` method, where the carve-out (since removed) masked the
// double-free. The hard double-free only manifests under INDIRECT dispatch
// (interface / virtual-override / imported / function-value Dispatch), where
// the call site cannot resolve the concrete borrow-returning method and falls
// to the name-head Owned fallback. #1760 added the def-site reject so the
// concrete method's `return _items;` could never compile, but that reject
// also forbade the §28.6 worked example (`return _transactions;` from
// `method GetTransactions(): OwningArray<Transaction>`, spec L7000–L7003).
// #2269 restored the carve-out per the spec admission; the indirect-dispatch
// double-free hazard the carve-out reintroduces is filed separately as a
// spec-amendment follow-up (not a definition-site rejection — the spec
// admits the pattern, the soundness adjudication belongs in the spec). This
// test pins the §28.6 admission under interface dispatch: the program
// COMPILES (the def-site carve-out admits `return field;` from `Holder.Get()`),
// even though the runtime double-free under `Consume`'s `using var t := src.Get();`
// remains. Tracking the runtime fix in a follow-up issue.
BEZEL_TEST(borrowed_owning_1008c, owning_array_borrow_return_via_interface_admitted_2269) {
    // Spec admission: the def-site carve-out admits the pattern (the
    // interface method's override body is `return field;` — a bare read of
    // an `owned` field of the receiver, exactly the §28.6 shape). The
    // runtime double-free under indirect dispatch is the open spec-amendment
    // follow-up. A compile-time assertion is the load-bearing one for the
    // spec-correctness half of the fix; the runtime half is the test the
    // follow-up will add.
    BEZEL_EXPECT_FALSE(compileFails(
        "module M; "
        "class Cell public constructor() end constructor; "
        "  destructor end destructor; end Cell; "
        "interface IArrSource "
        "  method Get(): OwningArray<Cell>; "
        "end IArrSource; "
        "class Holder : IArrSource "
        "  public owned field: OwningArray<Cell>; "
        "  public constructor(owned f: OwningArray<Cell>) field := f; end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> return field; end Get; "
        "end Holder; "
        "class P public static method Main(): int return 0; end Main; end P;"));
}

// (4) Tightening: a FRESH owning value through a non-`owned` return is rejected.
BEZEL_TEST(borrowed_owning_1008c, fresh_nonowned_return_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string(prelude) +
        "class P public static method Make(): OwningArray<Cell> "
        "    return new OwningArray<Cell>(); end Make; "
        "  public static method Main(): int return 0; end Main; end P;"));
}

// (5) A FRESH owning value through an `owned` return is clean and the caller
// disposes it exactly once → Cell.n == 0.
BEZEL_TEST(borrowed_owning_1008c, fresh_owned_return_disposed_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(prelude) +
        "class P public static method Make(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); a.Add(new Cell()); return a; end Make; "
        "  public static method Main(): int "
        "    do using var x := P.Make(); end; return Cell.n; "
        "  end Main; end P;"), 0);
}

// (6) Dangling: returning a field of a scope-bound LOCAL is rejected (the borrow
// would outlive its source — a use-after-free).
BEZEL_TEST(borrowed_owning_1008c, dangling_local_field_return_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string(prelude) +
        "class P public static method Bad(): OwningArray<Cell> "
        "    do using var lh := new Holder(); return lh._items; end; end Bad; "
        "  public static method Main(): int return 0; end Main; end P;"));
}

// (7) The owner keeps the full API on its OWN field (the §28.7 pattern is not
// broken): draining `this`'s field INSIDE the Holder is permitted. (The same
// `h._items.Drain()` from OUTSIDE — an external borrow — is rejected by the
// capability rule, as it must be.)
BEZEL_TEST(borrowed_owning_1008c, owner_drains_own_field_clean) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(prelude) +
        "class P public static method Main(): int "
        "  do using var h := new Holder(); var r := h.DrainSelf(); end; "
        "  return Cell.n; "                       // 0 — drained element disposed once
        "end Main; end P;"), 0);
}
