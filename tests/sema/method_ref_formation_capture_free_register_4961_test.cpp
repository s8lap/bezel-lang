// fix #4961 (spec §28.14, v0.307): forming a method reference over a
// capture-free-register subscribe position is a compile error. The
// capture-free register covers `ISharedEvent<T>.Subscribe` and
// `SharedEventSource<T>.{Subscribe, Add}` (stdlib §6.5; #4920 enforces
// the same admission at the handler side). A bare `src.Subscribe` or
// `Bus.Feed.Subscribe` produces a plain `(T) -> ()` value
// indistinguishable from a plain `EventSource<T>` handler — the
// capture-free annotation is erased — so an `es.Add(f)` on it later
// would launder the cross-domain subscriber-list entry through a
// value-type the register was meant to refuse. Reject at the formation
// site (`#4920`'s handler-side reject does not fire for a bare
// reference — the handler argument does not exist yet).
//
// T1 — `src.Subscribe` on a `using var src: SharedEventSource<int>`:
// rejects with the §28.14 "method reference" diagnostic. The bare
// reference over the register is exactly the formation #4961 names.
// T2 — `Bus.Feed.Subscribe` through the `ISharedEvent<T>` interface
// face (the subscribe-only public view): rejects with the same
// diagnostic. The face is the public subscription-point and is also
// a register.
// T3 — `src.Add` (SharedEventSource<T>.Add): rejects. Add is the
// producer-side equivalent of Subscribe and shares the register
// semantics (§6.5 admits Add on SharedEventSource<T> as a
// capture-free-register position).
// T4 — `src.Subscribe` inside an EXPRESSION context that is then
// CALLED at the same site (`src.Subscribe(h)`) — control: this is
// the call form, NOT a bare reference, and #4961 explicitly does not
// reject it. The §6.5 / §27.1.1 / §28.14 / #4920 call-site path runs
// instead and rejects when the handler is not capture-free; this
// CONTROL row pins that the call-form path is unaffected.
//
// REJECT rows assert the diagnostic text "method reference" (the
// literal text the conformance case
// `tests/conformance/rules/cases/s28_14_0018.bzl` pins). The diagnostic
// names §28.14 and stdlib §6.5 explicitly; a vacuous accept is
// impossible because the check is keyed on the receiver-type head
// name + member name — neither #4968's plain-event handler-slot
// (which checks the handler VALUE shape) nor #4920's capture-free
// admission (which checks the handler VALUE's capture freeness) emit
// the "method reference" wording on a bare-reference formation.
//
// FAIL-WITHOUT PIN ROWS — pin the formation check itself, not the
// call-side handler check. Each row uses a SHARE-EVENT TYPED receiver
// (declaration-less Named ref whose head name is the registered
// builtin) and forms the bare reference in a context where #4920
// cannot fire — pre-fix the bare reference had no exprType set
// (SharedEventSource<T> has no MethodSymbol for Subscribe/Add), so
// `lowerLocalDecl`'s #2434 zero-initialized fallback degraded to a
// WARNING and the program reached codegen with a malformed slot. The
// post-fix rows reject with "method reference" at the formation site.

#include <string>

#include "support/aot_support.hpp"  // runAot — AOT + native exec oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasDiag(const bezel::test::JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — bare `src.Subscribe` on a `using var src: SharedEventSource<int>`.
// ---------------------------------------------------------------------------
BEZEL_TEST(method_ref_formation_capture_free_register_4961,
           shared_eventsource_subscribe_bare_reference_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    var formed := src.Subscribe; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "method reference"));
}

// ---------------------------------------------------------------------------
// REJECT — bare `Bus.Feed.Subscribe` through the `ISharedEvent<T>` face.
// ---------------------------------------------------------------------------
BEZEL_TEST(method_ref_formation_capture_free_register_4961,
           isharedevent_subscribe_bare_reference_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class Bus "
        "  private static owned readonly _feed: SharedEventSource<int> := new(); "
        "  public static property Feed: ISharedEvent<int> => _feed; "
        "end Bus; "
        "class Program "
        "  public static method Main(): int "
        "    var formed := Bus.Feed.Subscribe; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "method reference"));
}

// ---------------------------------------------------------------------------
// REJECT — bare `src.Add` on a `using var src: SharedEventSource<int>`.
// ---------------------------------------------------------------------------
BEZEL_TEST(method_ref_formation_capture_free_register_4961,
           shared_eventsource_add_bare_reference_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    var formed := src.Add; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "method reference"));
}

// ---------------------------------------------------------------------------
// CONTROL — bare `src.Subscribe` inside a CALL (`src.Subscribe(h)`) is
// the CALL form, NOT a bare reference. The formation check does not fire;
// the §6.5 / §28.14 / #4920 call-side capture-free check (on `h`) does
// fire (h.Handle is a non-static bound method), and rejects with the
// §6.5 "must be capture-free" diagnostic — NOT the "method reference"
// diagnostic. This row pins that #4961's check does not bleed into
// the call path.
// ---------------------------------------------------------------------------
BEZEL_TEST(method_ref_formation_capture_free_register_4961,
           shared_eventsource_subscribe_call_form_unchanged_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class H "
        "  public method Handle(n: int) Console.WriteLine($\"v:{n}\"); end Handle; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    var h := new H(); "
        "    src.Subscribe(h.Handle); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    // The call form goes through #4920, not #4961. #4920 rejects
    // with the capture-free diagnostic (no "method reference" substring).
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "must be capture-free"));
    // and explicitly NOT the #4961 formation diagnostic.
    BEZEL_EXPECT_FALSE(hasDiag(r, "forming a method reference"));
}

// ===========================================================================
// FAIL-WITHOUT PIN ROWS — these rows pass with the fix (reject) and
// degrade-to-warning WITHOUT the fix. Pre-fix the bare reference had no
// exprType (SharedEventSource<T> has no MethodSymbol for Subscribe/Add),
// so `lowerLocalDecl` emitted #2434's zero-initialized fallback and
// codegen reached the malformed-slot state. Post-fix the rows reject
// at the formation site with the §28.14 diagnostic.
// ===========================================================================

// F1 — bare `src.Subscribe` as a DECLARED-TYPED local initializer
// (`var formed: (int) -> () := src.Subscribe`): pre-fix the
// declared-type form did NOT trigger the lowerLocalDecl warning
// (the declared type gave `formed` a Function slot), and the
// malformed `(int) -> ()` reached codegen through §27.1.1's
// assignability check (which permits an exact-shape Function to
// Function) — silently accepted. Post-fix the formation site
// rejects before §27.1.1 ever runs.
BEZEL_TEST(method_ref_formation_capture_free_register_4961,
           fail_without_shared_eventsource_subscribe_typed_local_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    var formed: (int) -> () := src.Subscribe; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "method reference"));
}

// F2 — bare `src.Subscribe` as an ASSIGNMENT VALUE
// (`var formed: (int) -> (); formed := src.Subscribe`): pre-fix the
// assignment rhs was untyped (same no-MethodSymbol gap), degraded
// to a zero-initialized value via §17.11 defaultable-local
// semantics, and the program silently stored a null function pointer.
// Post-fix the assignment rhs is rejected at the formation site.
BEZEL_TEST(method_ref_formation_capture_free_register_4961,
           fail_without_shared_eventsource_subscribe_assignment_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    var formed: (int) -> () := lambda(x: int) => Console.WriteLine($\"v:{x}\"); "
        "    formed := src.Subscribe; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "method reference"));
}

// F3 — bare `Bus.Feed.Subscribe` as a RETURN VALUE (`return
// Bus.Feed.Subscribe`): pre-fix the return was untyped, the
// enclosing method body was §17.11-zero-initialized for the
// implicit return slot, and codegen ran with a null function value
// (the §5.1 / §27.1.1 return-typed check accepts the same Function
// shape silently). Post-fix the return-position formation site is
// rejected.
BEZEL_TEST(method_ref_formation_capture_free_register_4961,
           fail_without_isharedevent_subscribe_return_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class Bus "
        "  private static owned readonly _feed: SharedEventSource<int> := new(); "
        "  public static property Feed: ISharedEvent<int> => _feed; "
        "end Bus; "
        "class Program "
        "  public static method Make(): (int) -> () "
        "    return Bus.Feed.Subscribe; "
        "  end Make; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "method reference"));
}
