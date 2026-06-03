// Spec-derived tests for the §24.3 event visibility fence.
//
// §24.3: "An event member has two faces, distinguished by whether the access
// is from inside the declaring class or from outside." Outside the declaring
// class the member's static type is `IEvent<T>` / `IAsyncEvent<T>`, which
// exposes ONLY `Subscribe`; the producer-side operations `Raise` /
// `RaiseAsync` / `Add` / `Remove` (and the `+=` / `-=` desugaring, §24.4) live
// on `EventSource<T>` / `AsyncEventSource<T>`, the in-class face. §24.5 adds:
// "Derived classes may raise `protected` events declared on their base class
// because the protected access exposes the EventSource form."
//
// These tests verify that sema accepts the producer-side operations inside the
// declaring class (and inside a derived class for a `protected` event) and
// diagnoses them from outside, while `Subscribe` is accepted everywhere.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// A reusable event-bearing class plus a payload record. `Button` declares a
// synchronous event; the `op` text is spliced into a second class that holds an
// external `Button` reference, so the access happens from OUTSIDE Button.
const char* kPrelude =
    "module M; "
    "record Ev(N: int); "
    "class Button "
    "  public event Clicked: Ev; ";

// Build a program where `Button` (the declaring class) runs `inside` in one of
// its own methods, and an external class `Other` runs `outside` against a
// `Button` parameter `b`.
std::string prog(const std::string& insideBody, const std::string& outsideBody) {
    return std::string(kPrelude) +
           "  public method Inside() " + insideBody + " end Inside; "
           "end Button; "
           "class Other "
           "  public method Outside(b: Button) " + outsideBody + " end Outside; "
           "end Other;";
}

} // namespace

// ── Inside the declaring class: the EventSource<T> face is in scope (§24.3). ──

// §24.5: `Clicked.Raise(...)` inside the declaring class is allowed.
BEZEL_TEST(sema_event_vis_24_3, raise_inside_declaring_class_ok) {
    auto r = parseSemaAll(prog(
        "Clicked.Raise(new Ev(1));",   // in-class: EventSource<T>.Raise
        "var _ := b;"));               // external method does nothing relevant
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §24.3 / §24.4: `Clicked += handler` inside the declaring class desugars to
// EventSource<T>.Add and is allowed.
BEZEL_TEST(sema_event_vis_24_3, plus_equals_inside_declaring_class_ok) {
    auto r = parseSemaAll(prog(
        "Clicked += (e) => do end;",   // in-class: += → Add
        "var _ := b;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §24.3: `Clicked -= handler` inside the declaring class desugars to
// EventSource<T>.Remove and is allowed.
BEZEL_TEST(sema_event_vis_24_3, minus_equals_inside_declaring_class_ok) {
    auto r = parseSemaAll(prog(
        "Clicked -= (e) => do end;",   // in-class: -= → Remove
        "var _ := b;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §24.3: explicit `Clicked.Add(handler)` inside the declaring class is allowed.
BEZEL_TEST(sema_event_vis_24_3, add_inside_declaring_class_ok) {
    auto r = parseSemaAll(prog(
        "Clicked.Add((e) => do end);", // in-class: EventSource<T>.Add
        "var _ := b;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §24.3: explicit `Clicked.Remove(handler)` inside the declaring class is ok.
BEZEL_TEST(sema_event_vis_24_3, remove_inside_declaring_class_ok) {
    auto r = parseSemaAll(prog(
        "Clicked.Remove((e) => do end);", // in-class: EventSource<T>.Remove
        "var _ := b;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── Outside the declaring class: only IEvent<T>.Subscribe is in scope. ──

// §24.5: "Calling Raise ... outside the declaring class is a compile error:
// the static type of the event member outside the declaring class is the
// interface, which has no Raise method."
BEZEL_TEST(sema_event_vis_24_3, raise_outside_declaring_class_diagnosed) {
    auto r = parseSemaAll(prog(
        "var _ := this;",
        "b.Clicked.Raise(new Ev(1));")); // outside: IEvent<T> has no Raise
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §24.3: `Add` is on EventSource<T>, not IEvent<T> — forbidden from outside.
BEZEL_TEST(sema_event_vis_24_3, add_outside_declaring_class_diagnosed) {
    auto r = parseSemaAll(prog(
        "var _ := this;",
        "b.Clicked.Add((e) => do end);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §24.3: `Remove` is on EventSource<T>, not IEvent<T> — forbidden from outside.
BEZEL_TEST(sema_event_vis_24_3, remove_outside_declaring_class_diagnosed) {
    auto r = parseSemaAll(prog(
        "var _ := this;",
        "b.Clicked.Remove((e) => do end);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §24.3 / §24.4: the `+=` desugaring (→ Add) is available only when the static
// type is EventSource<T>; from outside it is IEvent<T>, so it is forbidden.
BEZEL_TEST(sema_event_vis_24_3, plus_equals_outside_declaring_class_diagnosed) {
    auto r = parseSemaAll(prog(
        "var _ := this;",
        "b.Clicked += (e) => do end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §24.3 / §24.4: the `-=` desugaring (→ Remove) is likewise forbidden outside.
BEZEL_TEST(sema_event_vis_24_3, minus_equals_outside_declaring_class_diagnosed) {
    auto r = parseSemaAll(prog(
        "var _ := this;",
        "b.Clicked -= (e) => do end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §24.4 / §24.3: "Subscribe is available everywhere — inside or outside the
// declaring class — because it is on the public interface." So a `Subscribe`
// call from outside is NOT flagged by the visibility fence.
BEZEL_TEST(sema_event_vis_24_3, subscribe_outside_declaring_class_ok) {
    auto r = parseSemaAll(prog(
        "var _ := this;",
        "var s := b.Clicked.Subscribe((e) => do end);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §24.4: Subscribe from inside the declaring class is also fine.
BEZEL_TEST(sema_event_vis_24_3, subscribe_inside_declaring_class_ok) {
    auto r = parseSemaAll(prog(
        "var s := Clicked.Subscribe((e) => do end);",
        "var _ := b;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── §24.5 derived-class raising of a `protected` event. ──

// §24.5: "Derived classes may raise `protected` events declared on their base
// class because the protected access exposes the EventSource form." A subclass
// raising a base-class `protected` event is allowed.
BEZEL_TEST(sema_event_vis_24_3, derived_raise_protected_base_event_ok) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Base "
        "  protected event Clicked: Ev; "
        "end Base; "
        "class Derived: Base "
        "  public method Fire() Clicked.Raise(new Ev(1)); end Fire; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §24.5: the derived-class allowance is specific to `protected`. A `public`
// base event raised from a subclass is still outside its declaring class, so
// the producer-side face (Raise) is not in scope — it is diagnosed. (Public is
// the maximal visibility, so this confirms the fence keys on the declaring
// CLASS, not on whether the member is otherwise visible.)
BEZEL_TEST(sema_event_vis_24_3, derived_raise_public_base_event_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Base "
        "  public event Clicked: Ev; "
        "end Base; "
        "class Derived: Base "
        "  public method Fire() this.Clicked.Raise(new Ev(1)); end Fire; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
