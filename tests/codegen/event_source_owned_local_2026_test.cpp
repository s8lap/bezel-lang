// Codegen — owned EventSource<T>/AsyncEventSource<T> LOCAL scope-end disposal
// (stdlib §6.5 v0.187; spec §24.3 / §28.2 / §28.3; issue #2026).
//
// Before #2026 the codegen admitted an owned LOCAL of type EventSource<T> /
// AsyncEventSource<T> (`using var e := new EventSource<T>();` — stdlib §6.5 v0.187
// made the constructors public) but never registered it for scope-end disposal:
// EventSource has no user TypeSymbol / no vtable, so classOfLocal returns null
// and every prior builtin-disposal arm (Subscription / Task / Channel / Shared /
// Buffer / …) missed the type by name. The owned-FIELD path
// (emitOwnedFieldDisposals via isOwnedEventSourceField, #1646) already freed
// such a source through bz_event_source_free; the owned-LOCAL path was simply
// not wired. A `using var e := new EventSource<T>();` therefore leaked its
// 32-byte bz_event_source_t (plus its handler array once subscribed) at scope
// end on every shape — direct `new`, a factory call returning EventSource<T>
// (the #2017 generic-factory shape), and AsyncEventSource<T>.
//
// The fix adds a new Disposal arm (registerUsingDisposal + emitDisposals in
// LlvmBackend_{Setup,Disposal}.cpp) keyed on the builtin type names
// "EventSource" / "AsyncEventSource" (declared type, NewExpr type, OR inferred
// initializer exprType — the generic-factory shape). At scope end it calls
// bz_event_source_free (null-safe — reclaims the subscriber list and the
// struct; the borrowed handler closures are owned by their Subscriptions and
// are disposed separately). The slot is nulled afterward so a re-entrant or
// unwind pass is a guarded no-op — exactly-once, no double-free.
//
// Spec authority:
//   - §28.2 (IDisposable contract): every class instance is disposed exactly
//     once; the compiler-generated Dispose is idempotent (so the local's
//     scope-end free, distinct from any field disposal, runs exactly once
//     per scope exit).
//   - §28.3 K3.14 (Affinity): "An owned value that reaches its scope's end
//     undischarged and not moved-from is disposed there implicitly" — the
//     `using var` local discharges at scope end via this arm.
//   - §24.3 / stdlib §6.5 (v0.187): the producer-side source is constructed
//     directly via `new EventSource<T>()`, and "the source's destructor severs
//     the subscriber list before reclamation" (quiescence).
//
// Oracle: runAotChecked compiles with generated-code AddressSanitizer and runs
// under LeakSanitizer, so a leak / double-free / use-after-free of the owned
// local source flips r.ran to false (-> -999). Each case FAILS without the fix
// (the local leaks; LSan reports the 32-byte bz_event_source_t allocation) and
// PASSES with it.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Compile with generated-code ASan, run under LSan, return the exit code —
// the construct+subscribe+raise+observe AND dispose-exactly-once oracle in one
// run. A leak / double-free / use-after-free FAILS the run (ran=false -> -999).
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A static accumulator (Total._sum) that subscribed handlers add to, so handler
// firing is observable through the program's exit code. Shared preamble.
const char* kPreamble =
    "module M;\n"
    "import System.Threading;\n"
    "record TickEvent(N: int);\n"
    "class Total\n"
    "  public static _sum: int := 0;\n"
    "  public static method Add(n: int) _sum := _sum + n; end Add;\n"
    "end Total;\n";

// Shape 1 — the issue's exact repro shape: a `using var e := new
// EventSource<T>();` LOCAL (not a field). Subscribe one handler, raise once,
// observe it fires. The owned local must be freed at scope end via
// bz_event_source_free (pre-#2026 it leaked the 32-byte bz_event_source_t and
// its 96-byte handler array). sum = 7.
const std::string kOwnedLocalDirect =
    std::string(kPreamble) +
    "class C\n"
    "  public static method Main(): int\n"
    "    do\n"
    "      // per §28.3 K3.14 / §24.3: an owned EventSource<T> local — the\n"
    "      // producer-side source constructed directly (stdlib §6.5 v0.187).\n"
    "      // #2026: scope-end disposal must call bz_event_source_free.\n"
    "      using var e := new EventSource<TickEvent>();\n"
    "      using var s := e.Subscribe((ev) => do Total.Add(ev.N); end);\n"
    "      e.Raise(new TickEvent(7));\n"
    "    end;\n"
    "    return Total._sum;\n"
    "  end Main;\n"
    "end C;\n";

// Shape 2 — the #2017 generic-factory shape: a `using var` initialized from a
// call returning EventSource<T> (the NewExpr-detection branch alone does not
// cover this; the inferred-exprType detection arm does). The owned local is
// likewise disposed at scope end via bz_event_source_free. sum = 9.
const std::string kOwnedLocalFactory =
    std::string(kPreamble) +
    "class SourceFactory\n"
    "  public static method Make(): owned EventSource<TickEvent>\n"
    "    return new EventSource<TickEvent>();\n"
    "  end Make;\n"
    "end SourceFactory;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    do\n"
    "      // per §28.3: an owned local produced by a factory call — the\n"
    "      // #2017 generic-factory path. #2026: scope-end free applies here\n"
    "      // too (inferred exprType name detection).\n"
    "      using var e := SourceFactory.Make();\n"
    "      using var s := e.Subscribe((ev) => do Total.Add(ev.N); end);\n"
    "      e.Raise(new TickEvent(9));\n"
    "    end;\n"
    "    return Total._sum;\n"
    "  end Main;\n"
    "end C;\n";

// Shape 3 — AsyncEventSource<T> shares the same bz_event_source_t runtime
// representation and the same scope-end free path. Construct via `using var`,
// subscribe an async handler, RaiseAsync, observe the handler firing. sum = 5.
const std::string kOwnedLocalAsync =
    std::string(kPreamble) +
    "class C\n"
    "  public static async method Run(): Task\n"
    "    do\n"
    "      // per §24.3 / stdlib §6.5: an owned AsyncEventSource<T> local —\n"
    "      // same runtime object as the sync source, freed identically at\n"
    "      // scope end via bz_event_source_free (#2026).\n"
    "      using var e := new AsyncEventSource<TickEvent>();\n"
    "      using var s := e.Subscribe(async (ev) => do Total.Add(ev.N); end);\n"
    "      await e.RaiseAsync(new TickEvent(5));\n"
    "    end;\n"
    "  end Run;\n"
    "  public static method Main(): int\n"
    "    Run().Wait();\n"
    "    return Total._sum;\n"
    "  end Main;\n"
    "end C;\n";

}  // namespace

// §28.2 / §28.3 / §24.3 / stdlib §6.5 (v0.187) — a `using var e := new
// EventSource<T>();` LOCAL is functional (handlers fire) and freed leak-clean
// at scope end via bz_event_source_free. Before #2026: leaked 32 B + 96 B.
BEZEL_TEST(codegen_event_source_owned_local_2026,
           direct_new_local_subscribes_raises_freed_leak_free_spec_28_2_24_3) {
    BEZEL_EXPECT_EQ(run(kOwnedLocalDirect), 7);
}

// §28.3 — a `using var e := Factory();` returning owned EventSource<T> covers
// the #2017 generic-factory shape; inferred-exprType detection routes the
// local to the same scope-end free. Before #2026: leaked 32 B.
BEZEL_TEST(codegen_event_source_owned_local_2026,
           factory_local_subscribes_raises_freed_leak_free_spec_28_3) {
    BEZEL_EXPECT_EQ(run(kOwnedLocalFactory), 9);
}

// §24.3 / stdlib §6.5 (v0.187) — AsyncEventSource<T> shares the runtime
// representation and the same scope-end free path. Before #2026: leaked 32 B.
BEZEL_TEST(codegen_event_source_owned_local_2026,
           async_local_subscribes_raises_async_freed_leak_free_spec_24_3) {
    BEZEL_EXPECT_EQ(run(kOwnedLocalAsync), 5);
}
