// Codegen — sema + codegen admit SharedEventSource<T> / ISharedEvent<T>
// (stdlib §6.5, v0.187; spec §24.3; §22.12 rule 5; §28.14; issue #1832).
//
// stdlib §6.5 (v0.187) declares a *shared* (cross-domain) publication pair
// `SharedEventSource<T>` / `ISharedEvent<T>`. Before #1832 the compiler did
// not implement them at all: any use of the type names was rejected at sema
// with `unresolved type name SharedEventSource` (or `ISharedEvent`). This is a
// conforming *rejection* (not a miscompile), but the surface was
// unimplemented — a shared event source needs its type-name registered, a
// construction entry point, and disposal wiring. The synchronized runtime
// object (subscriber list guarded for cross-domain subscribe/raise/
// Subscription.Dispose) is recorded as a follow-up slice; this fix admits
// the type names, lowers `new SharedEventSource<T>()` through
// bz_shared_event_source_new (a sibling of bz_event_source_new), and wires
// disposal through bz_shared_event_source_free (a sibling of
// bz_event_source_free). Method dispatch on the SharedEventSource<T> class
// face is not part of this fix; the ISharedEvent<T> / SharedEventSource<T>
// type NAMES are.
//
// Three failure shapes are pinned by this test, each as the oracle:
//   (1) a parameter type `SharedEventSource<T>` — the type-name resolution
//       path (builtinTypeArity / kHeapPointerReferenceBuiltins /
//       kBuiltinInterfaceHeads / kDisposableBuiltinHeadNames /
//       kBuiltinClassModulePaths). Pre-#1832: `unresolved type name
//       SharedEventSource`. Post-#1832: compiles + links + runs (the
//       parameter is unused, so the disposal concern does not apply — the
//       test pins pure admission).
//   (2) a constructor-bodied `owned readonly` field assignment
//       `_s := new SharedEventSource<T>();`. Pre-#1832: `unresolved type name
//       SharedEventSource`. Post-#1832: compiles + lowers to
//       bz_shared_event_source_new, and the owner-disposal arm frees the
//       field via bz_shared_event_source_free at dispose time.
//   (3) a `using var s := new SharedEventSource<T>();` LOCAL — pre-#1832:
//       `unresolved type name SharedEventSource`. Post-#1832: scope-end
//       disposal frees the local via bz_shared_event_source_free.
//
// Spec authority:
//   - stdlib §6.5 (v0.187): the SharedEventSource<T> / ISharedEvent<T> pair
//     is the cross-domain publication point, sealed, no IEvent<T>.
//   - §22.12 rule 5: static reachability is the distribution mechanism.
//   - §28.14: capture-free subscribe position; we do NOT exercise it here
//     (method dispatch on SharedEventSource<T> is a follow-up slice — the
//     type names being admitted is the issue's primary complaint).
//   - §28.3 / §28.5 / §28.9: the field/local disposal obligations this fix
//     wires.
//
// Oracle: runAot compiles + links + runs to a stable exit code; pre-#1832
// bezelc REJECTS the test programs at compile time (sema), so the same
// programs report ran=false (-> -999) without the fix and the expected exit
// code with it.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Compile + run via the real driver; return the exit code on success, -999
// on compile/link/run failure. The compile failure is the FAIL-WITHOUT
// signal: pre-#1832 the bezelc invocation returns ran=false (sema rejected
// the type name).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Shared preamble: the import that gates the SharedEventSource / ISharedEvent
// names per stdlib §2.3 (System.Threading exposes System per the §2.3 auto-
// import rule).
const char* kPreamble =
    "module M;\n"
    "import System.Threading;\n"
    "record TickEvent(N: int);\n";

// Shape (1) — parameter type admission (no construction, no disposal). The
// minimal proof that the type name is now admitted by sema. The unused
// parameter exercises only the type-name resolution path
// (builtinTypeArity → kHeapPointerReferenceBuiltins → isBuiltinReferenceType).
const std::string kParameterAdmission =
    std::string(kPreamble) +
    "class C\n"
    "  // fix #1832 / §6.5 (v0.187): SharedEventSource<T> is admitted as a\n"
    "  // parameter type — pre-fix the name was rejected at sema with\n"
    "  // `unresolved type name SharedEventSource`.\n"
    "  public static method Helper(s: SharedEventSource<TickEvent>) end Helper;\n"
    "  public static method Main(): int\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Shape (2) — ctor-body assigned `owned readonly` field. Mirrors the
// #1646 instance-source shape for EventSource<T>; here it's the shared
// cross-domain pair. The owner-disposal arm wires the field through
// bz_shared_event_source_free at dispose time (isOwnedSharedEventSourceField).
const std::string kFieldAssignment =
    std::string(kPreamble) +
    "class Bus\n"
    "  private owned readonly _s: SharedEventSource<TickEvent>;\n"
    "  public constructor()\n"
    "    // fix #1832: `new SharedEventSource<T>()` lowers to\n"
    "    // bz_shared_event_source_new (a sibling of bz_event_source_new),\n"
    "    // and the field is freed via bz_shared_event_source_free when\n"
    "    // the owning instance is reclaimed.\n"
    "    _s := new SharedEventSource<TickEvent>();\n"
    "  end constructor;\n"
    "end Bus;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    do\n"
    "      using var b := new Bus();\n"
    "    end;\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Shape (3) — `using var` LOCAL of SharedEventSource<T>. Mirrors the
// #2026 instance-source local shape; here the local's scope-end disposal
// calls bz_shared_event_source_free (registerUsingDisposal / emitDisposals
// `sharedEventSource` arm).
const std::string kOwnedLocalDirect =
    std::string(kPreamble) +
    "class C\n"
    "  public static method Main(): int\n"
    "    do\n"
    "      // fix #1832 / §28.3 K3.14: an owned SharedEventSource<T> local\n"
    "      // is constructed via bz_shared_event_source_new and freed at\n"
    "      // scope end via bz_shared_event_source_free (null-safe).\n"
    "      using var s := new SharedEventSource<TickEvent>();\n"
    "    end;\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Shape (4) — the canonical §6.5 (v0.187) pattern: `owned readonly` field
// with the §17.7 `:= new()` shorthand, plus the §23.2 expression-bodied
// ISharedEvent<T>-face property. This is the shape stdlib §6.5 stamps as the
// idiomatic static-reachable cross-domain publication point. Both lowering
// paths exercised at once: bz_shared_event_source_new on construction and
// bz_shared_event_source_free on owner reclaim.
const std::string kFieldShorthandPattern =
    std::string(kPreamble) +
    "class Bus\n"
    "  private owned readonly _feed: SharedEventSource<TickEvent> := new();\n"
    "  public property Feed: ISharedEvent<TickEvent> => _feed;\n"
    "end Bus;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    do\n"
    "      using var b := new Bus();\n"
    "    end;\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

}  // namespace

// stdlib §6.5 (v0.187) / fix #1832 / §2.3 — the type-name SharedEventSource<T>
// is admitted by sema (pre-fix: `unresolved type name SharedEventSource`).
BEZEL_TEST(codegen_shared_event_source_admit_1832,
           parameter_type_admitted_spec_6_5_v0_187) {
    BEZEL_EXPECT_EQ(run(kParameterAdmission), 0);
}

// stdlib §6.5 (v0.187) / fix #1832 / §24.3 / §28.5 — ctor-body assigned
// `owned readonly` field: `new SharedEventSource<T>()` lowers to
// bz_shared_event_source_new, and the owner-disposal arm frees the field via
// bz_shared_event_source_free when the owning instance is reclaimed.
BEZEL_TEST(codegen_shared_event_source_admit_1832,
           field_assignment_lowers_and_disposes_spec_6_5_28_5) {
    BEZEL_EXPECT_EQ(run(kFieldAssignment), 0);
}

// stdlib §6.5 (v0.187) / fix #1832 / §28.3 — `using var` LOCAL of
// SharedEventSource<T>: bz_shared_event_source_new at construction,
// bz_shared_event_source_free at scope end (the registerUsingDisposal
// `sharedEventSource` arm).
BEZEL_TEST(codegen_shared_event_source_admit_1832,
           using_var_local_constructs_and_frees_spec_6_5_28_3) {
    BEZEL_EXPECT_EQ(run(kOwnedLocalDirect), 0);
}

// stdlib §6.5 (v0.187) / fix #1832 / §23.2 / §28.5 — the canonical pattern:
// `owned readonly` field with the §17.7 `:= new()` shorthand + an
// ISharedEvent<T>-face property. Exercises both lowering paths at once
// (bz_shared_event_source_new + bz_shared_event_source_free on owner reclaim).
BEZEL_TEST(codegen_shared_event_source_admit_1832,
           field_shorthand_pattern_constructs_and_disposes_spec_6_5_28_5) {
    BEZEL_EXPECT_EQ(run(kFieldShorthandPattern), 0);
}