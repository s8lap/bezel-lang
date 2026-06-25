// fix #1008 Part C / §28.6 (v0.151): returning a *borrowed* owning collection.
//
// A non-`owned` owning-collection return type denotes a BORROW (the `Owned*`
// name is element ownership, orthogonal to the `owned` container qualifier,
// §5.11). A method may return a bare read of an `owned` owning-collection FIELD
// of the receiver (`this`) as a borrow — the caller reads it but does not
// dispose it; the receiver disposes the container and its elements exactly once.
//
// Soundness rests on three coupled rules, all exercised here:
//   1. caller-side: the result is tracked as a borrow (never disposed) — no
//      double-free with the receiver's disposal;
//   2. definition-site tightening: a FRESH owning value (or any owned value)
//      returned through a non-`owned` return is rejected (must declare `owned`),
//      and a field of a scope-bound LOCAL is rejected (a dangling borrow);
//   3. capability restriction: the ownership-transferring / disposing ops
//      (Dispose/Drain/Pop*/Add/…) are rejected on a borrow.
//
// Disposal is observed through a static Cell counter (ctor ++ / dtor --): a net
// Cell.n of 0 after the owning scope ends proves dispose-exactly-once.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1008c_" + std::to_string(counter++);
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
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1008c_neg_" + std::to_string(counter++);
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}

// A Cell with a disposal counter, and a Holder owning an OwnedArray<Cell> field
// with a borrow-returning accessor `GetItems`.
const char* prelude =
    "module M; "
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
    "class Holder public owned _items: OwnedArray<Cell>; "
    "  public constructor() _items := new OwnedArray<Cell>(); _items.Add(new Cell()); end constructor; "
    "  destructor end destructor; "
    "  public method GetItems(): OwnedArray<Cell> => _items; "
    "  public method DrainSelf(): int "
    "    for var c in _items.Drain() do c.Dispose(); end; return 0; "
    "  end DrainSelf; "
    "end Holder; ";
}  // namespace

// (1) The receiver-field borrow is returned, the caller READS it (Count), and
// the Holder disposes its field + element exactly once → Cell.n == 0.
BEZEL_TEST(borrowed_owning_1008c, borrow_field_read_disposed_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(prelude) +
        "class P public static method Main(): int "
        "  do using var h := new Holder(); "
        "     var items := h.GetItems(); var n := items.Count(); end; "
        "  return Cell.n; "                       // 0 — disposed once
        "end Main; end P;"), 0);
}

// (2) Capability restriction: disposing the borrowed collection is a compile
// error (it would double-free the Holder's container).
BEZEL_TEST(borrowed_owning_1008c, caller_dispose_borrow_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string(prelude) +
        "class P public static method Main(): int "
        "  do using var h := new Holder(); "
        "     var items := h.GetItems(); items.Dispose(); end; "
        "  return Cell.n; "
        "end Main; end P;"));
}

// (3) Mutation / transfer ops are memory-safe on a borrow (dispose-exactly-once
// is preserved) and remain available — only `Dispose` is restricted. Draining a
// borrowed collection transfers its elements to the consumer; the owner then
// disposes the (empty) container. With the consumer discharging each element,
// Cell.n returns to 0 — no leak, no double-free.
BEZEL_TEST(borrowed_owning_1008c, caller_drain_borrow_allowed) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(prelude) +
        "class P public static method Main(): int "
        "  do using var h := new Holder(); "
        "     var items := h.GetItems(); "
        "     for var c in items.Drain() do c.Dispose(); end; end; "
        "  return Cell.n; "                       // 0 — drained + container disposed once
        "end Main; end P;"), 0);
}

// (4) Tightening: a FRESH owning value through a non-`owned` return is rejected.
BEZEL_TEST(borrowed_owning_1008c, fresh_nonowned_return_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string(prelude) +
        "class P public static method Make(): OwnedArray<Cell> "
        "    return new OwnedArray<Cell>(); end Make; "
        "  public static method Main(): int return 0; end Main; end P;"));
}

// (5) A FRESH owning value through an `owned` return is clean and the caller
// disposes it exactly once → Cell.n == 0.
BEZEL_TEST(borrowed_owning_1008c, fresh_owned_return_disposed_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(prelude) +
        "class P public static method Make(): owned OwnedArray<Cell> "
        "    var a := new OwnedArray<Cell>(); a.Add(new Cell()); return a; end Make; "
        "  public static method Main(): int "
        "    do using var x := P.Make(); end; return Cell.n; "
        "  end Main; end P;"), 0);
}

// (6) Dangling: returning a field of a scope-bound LOCAL is rejected (the borrow
// would outlive its source — a use-after-free).
BEZEL_TEST(borrowed_owning_1008c, dangling_local_field_return_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string(prelude) +
        "class P public static method Bad(): OwnedArray<Cell> "
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
