// Codegen — issue #2170 (§30.5): reading a block-bodied getter
// (`get => do ... end;`) from an ASYNC method emitted malformed IR
// ("Referring to an argument in another function!" / IR verification failure).
//
// Root cause: ensurePropertyBlockGetter (and ensurePropertyAccessorFn) saved
// and restored the emitter's per-method state (currentFn_, locals_, currentThis_,
// etc.) when switching into the standalone getter function, but did NOT save
// the state-machine fields smKind_ and smStateStruct_. With those still set to
// the CALLER's async state (Await + a real state struct), the getter body's
// `return _n` tripped the async-return path's gate
//   `if (smKind_ == SuspendKind::Await && smStateStruct_)` in
//   LlvmBackend_Stmts.cpp (~line 1510)
// and emitted bz_task_complete + spill-free + frame-free INTO the getter
// function. The getter's own `ret i32` was never emitted, the returned value
// matched the WRONG function's signature, and the verifier rejected the module
// ("Function return type does not match operand type of return inst!").
//
// The fix adds smKind_/smStateStruct_ to the saved/restored state in both
// ensurePropertyBlockGetter and ensurePropertyAccessorFn, and resets both to
// non-async-mode for the duration of the body emission.
//
// Spec derivation:
//   §23.3 — block-bodied accessor (`get => do … end;`).
//   §30.5 — async method return completes the outer task; the completion
//            pathway must not fire inside a non-async helper function.
//   §22.9 — property accessors are standalone functions; a `return` inside a
//            block getter terminates the getter, not its caller.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// §30.5 / #2170 — primary repro (issue body): block-bodied int getter read
// from an async method that owns the receiver with `using var`. Before the
// fix the AOT compile failed with "Referring to an argument in another
// function!" / "Function return type does not match operand type of return
// inst!". After the fix it compiles and returns 0 (= o.N - 7 with o.N == 7).
BEZEL_TEST(async_block_getter_2170, block_getter_read_from_async_method) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Owner\n"
        "  private _n: int;\n"
        "  public constructor() _n := 7; end constructor;\n"
        "  public property N: int\n"
        "    get => do\n"
        "      return _n;\n"
        "    end;\n"
        "  end N;\n"
        "end Owner;\n"
        "class P\n"
        "  public static async method RunAsync(): Task<int>\n"
        "    using var o := new Owner();\n"
        "    return o.N;\n"
        "  end RunAsync;\n"
        "  public static async method Main(): Task<int>\n"
        "    using var t := P.RunAsync();\n"
        "    var r := await t;\n"
        "    return r - 7;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}

// Regression guard: block-bodied getter with multiple statements + local
// binding, still inside an async method. Confirms the body emission
// (block inlining) does not regress when it contains more than a single
// `return _n`.
BEZEL_TEST(async_block_getter_2170, block_getter_multi_stmt_from_async) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Counter\n"
        "  private _n: int;\n"
        "  public constructor() _n := 1; end constructor;\n"
        "  public property N: int\n"
        "    get => do\n"
        "      var x := _n;\n"
        "      x := x + 100;\n"
        "      return x;\n"
        "    end;\n"
        "  end N;\n"
        "end Counter;\n"
        "class P\n"
        "  public static async method RunAsync(): Task<int>\n"
        "    using var c := new Counter();\n"
        "    return c.N;\n"
        "  end RunAsync;\n"
        "  public static async method Main(): Task<int>\n"
        "    using var t := P.RunAsync();\n"
        "    var r := await t;\n"
        "    return r - 101;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}

// Control: same property shape but read from a SYNCHRONOUS method. Must
// remain clean — the fix is non-regressing for the existing path.
BEZEL_TEST(async_block_getter_2170, block_getter_from_sync_control) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Counter\n"
        "  private _n: int;\n"
        "  public constructor() _n := 5; end constructor;\n"
        "  public property N: int\n"
        "    get => do\n"
        "      return _n;\n"
        "    end;\n"
        "  end N;\n"
        "end Counter;\n"
        "class P\n"
        "  public static method Main(): int\n"
        "    var c := new Counter();\n"
        "    return c.N - 5;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}