// Codegen — issue #2274 item 4 (§31.2 / §29.7): a block-bodied property or
// indexer getter emitted from INSIDE a `try … finally` destroyed the enclosing
// finally registrations.
//
// Root cause: `ensurePropertyBlockGetter` / `ensureIndexerBlockGetter`
// (src/codegen/LlvmBackend_Assign.cpp) switch the emitter into a standalone
// getter function and clear the per-method state — `loopStack_.clear();
// finallyStack_.clear();` — but only `loopStack_` was SAVED and restored. The
// caller's `finallyStack_` frames (pushed by `lowerTry`) were therefore gone
// for good once a block getter was lowered inside a try body:
//
//   * `lowerTry`'s own post-body `finallyStack_.back()` / `pop_back()`
//     (LlvmBackend_Stmts.cpp ~2939) then ran on an EMPTY vector — an abort
//     under libstdc++ assertions, a size_ underflow (UB) without them; and
//   * every `return` / `break` / `continue` leaving that try consults
//     `finallyStack_` to decide whether to route through the finally, so with
//     the stack emptied it emitted NO finally at all.
//
// Spec derivation — §31.2 `try`/`catch`/`finally` (v0.228, spec line 8723):
//   "`finally` runs whether the protected block completed normally, was exited
//    by an exception (caught or uncaught), or was exited by `return` — or by a
//    `break` or `continue` targeting an enclosing loop."
// The finally's SIDE EFFECT (a static counter folded into the exit code) is the
// observable here, not merely that the program compiles.
//
// §29.7 (#623): `finallyBodyStack_` is documented as "always the same depth as
// finallyStack_", so the fix clears and restores it in lockstep.
//
// Each test below FAILS without the fix (`runAot` reports ran=false — the
// forked compile aborts on `finallyStack_.back()` — so `run()` yields -999) and
// PASSES with it, returning the finally-derived counter.
//
// The block getter is read as the FIRST reference to the property, inside the
// try body: `propBlockGetterFns_` caches the emitted function, so only the
// first read runs the save/clear/restore sequence under test.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §31.2 — "exited by `return`": the try body reads a block getter (which emits
// the standalone getter function mid-body) and then returns. The finally must
// still run: _fin == 1 → 1*10 + (7-7) == 10.
BEZEL_TEST(finally_block_getter_2274, return_out_of_try_runs_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Counter\n"
        "  private _n: int;\n"
        "  public constructor() _n := 7; end constructor;\n"
        "  public property N: int\n"
        "    get => do\n"
        "      return _n;\n"
        "    end;\n"
        "  end N;\n"
        "end Counter;\n"
        "class P\n"
        "  public static _fin: int := 0;\n"
        "  public static method Work(): int\n"
        "    using var c := new Counter();\n"
        "    try\n"
        "      var x := c.N;\n"        // first read → getter emitted here
        "      return x;\n"            // §31.2: must route through the finally
        "    finally\n"
        "      _fin := _fin + 1;\n"
        "    end;\n"
        "  end Work;\n"
        "  public static method Main(): int\n"
        "    var w := P.Work();\n"
        "    return _fin * 10 + (w - 7);\n"
        "  end Main;\n"
        "end P;\n"), 10);
}

// §31.2 — "exited by a `break` … targeting an enclosing loop": the loop
// encloses the try, so the break EXITS the try and must run the finally.
// i=1: n += 3, no break, finally → _fin 1. i=2: n += 3, break, finally →
// _fin 2. Result 2*10 + 6 == 26. Without the fix the break bypassed the
// finally entirely (and the compile aborted first).
BEZEL_TEST(finally_block_getter_2274, break_out_of_try_runs_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Counter\n"
        "  private _n: int;\n"
        "  public constructor() _n := 3; end constructor;\n"
        "  public property N: int\n"
        "    get => do\n"
        "      return _n;\n"
        "    end;\n"
        "  end N;\n"
        "end Counter;\n"
        "class P\n"
        "  public static _fin: int := 0;\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    var n := 0;\n"
        "    var i := 0;\n"
        "    while i < 5 do\n"
        "      i := i + 1;\n"
        "      try\n"
        "        n := n + c.N;\n"      // first read → getter emitted inside try
        "        if i = 2 then break; end;\n"
        "      finally\n"
        "        _fin := _fin + 1;\n"
        "      end;\n"
        "    end;\n"
        "    return _fin * 10 + n;\n"
        "  end Main;\n"
        "end P;\n"), 26);
}

// §31.2 — "exited by an exception (caught or uncaught)": the exception edge is
// structural (unwindTarget_), not driven by finallyStack_, so it must run the
// finally EXACTLY ONCE both before and after the fix — a double-run guard.
// _fin == 1, n == 4 + 1 → 1*10 + 5 == 15. runAotChecked adds the ASan/LSan
// oracle over the owned receiver crossing the unwind edge.
BEZEL_TEST(finally_block_getter_2274, throw_out_of_try_runs_finally_once) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M;\n"
        "class E end E;\n"
        "class Counter\n"
        "  private _n: int;\n"
        "  public constructor() _n := 4; end constructor;\n"
        "  public property N: int\n"
        "    get => do\n"
        "      return _n;\n"
        "    end;\n"
        "  end N;\n"
        "end Counter;\n"
        "class P\n"
        "  public static _fin: int := 0;\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    var n := 0;\n"
        "    try\n"
        "      try\n"
        "        n := n + c.N;\n"
        "        throw new E();\n"
        "      finally\n"
        "        _fin := _fin + 1;\n"   // exactly once, not zero and not twice
        "      end;\n"
        "    catch\n"
        "      E => n := n + 1;\n"
        "    end;\n"
        "    return _fin * 10 + n;\n"
        "  end Main;\n"
        "end P;\n"), 15);
}

// §31.2 with NESTED try/finally: the getter is emitted inside the INNER try,
// but the `return` exits BOTH — the restore must hand the OUTER frame back, not
// just leave the stack empty. Inner finally +1, outer finally +100 → 101.
BEZEL_TEST(finally_block_getter_2274, return_through_nested_finallys) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Counter\n"
        "  private _n: int;\n"
        "  public constructor() _n := 7; end constructor;\n"
        "  public property N: int\n"
        "    get => do\n"
        "      return _n;\n"
        "    end;\n"
        "  end N;\n"
        "end Counter;\n"
        "class P\n"
        "  public static _fin: int := 0;\n"
        "  public static method Work(): int\n"
        "    using var c := new Counter();\n"
        "    try\n"
        "      try\n"
        "        var x := c.N;\n"
        "        return x;\n"
        "      finally\n"
        "        _fin := _fin + 1;\n"
        "      end;\n"
        "    finally\n"
        "      _fin := _fin + 100;\n"
        "    end;\n"
        "  end Work;\n"
        "  public static method Main(): int\n"
        "    var w := P.Work();\n"
        "    return _fin + (w - 7);\n"
        "  end Main;\n"
        "end P;\n"), 101);
}

// Same defect in the sibling emitter `ensureIndexerBlockGetter` (§23.5 block-
// bodied INDEXER getter): reading `b[2]` inside the try emitted the indexer
// getter function and cleared the same stack. _fin == 1 → 1*10 + 0 == 10.
BEZEL_TEST(finally_block_getter_2274, indexer_block_getter_return_runs_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Box\n"
        "  private _d: [4]int;\n"
        "  public property [i: int]: int\n"
        "    get => do\n"
        "      return _d[i] + 7;\n"
        "    end;\n"
        "  end;\n"
        "end Box;\n"
        "class P\n"
        "  public static _fin: int := 0;\n"
        "  public static method Work(): int\n"
        "    using var b := new Box();\n"
        "    try\n"
        "      var x := b[2];\n"
        "      return x;\n"
        "    finally\n"
        "      _fin := _fin + 1;\n"
        "    end;\n"
        "  end Work;\n"
        "  public static method Main(): int\n"
        "    var w := P.Work();\n"
        "    return _fin * 10 + (w - 7);\n"
        "  end Main;\n"
        "end P;\n"), 10);
}

// Control: the same property read OUTSIDE any try — the save/restore must be a
// no-op there (the pre-existing path). 7 - 7 == 0.
BEZEL_TEST(finally_block_getter_2274, block_getter_outside_try_control) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Counter\n"
        "  private _n: int;\n"
        "  public constructor() _n := 7; end constructor;\n"
        "  public property N: int\n"
        "    get => do\n"
        "      return _n;\n"
        "    end;\n"
        "  end N;\n"
        "end Counter;\n"
        "class P\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    return c.N - 7;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}
