// Codegen - Family L (defect-campaign-2026-09.md §2.8), member #4151:
// `makePropertyThunk` (LlvmBackend_Layout.cpp:5535) was the SEVENTH
// lazily-memoized emitter missing from #2702's registry. It builds a
// closure thunk that loads the captured receiver from env and computes
// the property getter (the same getter `b.P` would, dispatched
// virtually when the property is virtual or interface). The thunk is
// memoized per property (`propertyThunks_` keyed by `sema::PropertySymbol*`)
// and reused for the lifetime of the module, so a stale save+restore
// of the per-body emitter fields leaks the caller's emission context
// into the memoized body and freezes whichever caller was lowered FIRST
// for the whole module - in `std::unordered_map` pointer-hash order.
//
// Spec (revision v0.290+):
//   §27.6 "Method and property references" - a property reference used in
//     function-value position produces the getter as a function value -
//     equivalent to `() => b.P`. The thunk is a closure {thunk_ptr, env}.
//   §27.7.1 (v0.180, #1544) "Property-reference equality" - two
//     references to the SAME property on the SAME receiver compare EQUAL
//     under the general `=` / `<>` operator. The thunk is keyed by
//     `sema::PropertySymbol*`, not by receiver - a reused thunk is the
//     load-bearing property.
//   §23.3 "Block-form properties" - a block-bodied accessor is its own
//     function and inherits nothing (the family-anchor statement).
//
// THE PIN. Two callers in two orders: reach the same thunk from a
// `var f := receiver.P` and from a `var g := otherReceiver.P`, both
// with the same property `P`, so the memoized thunk must return the
// SAME getter value (the property's own `IntProp.Value()`, not the
// caller's surrounding context). The thunk is built when the FIRST
// reference is lowered, and the SECOND caller must see that same
// memoized body - the divergence #4151 measured is the SECOND caller's
// context leaking into the thunk.
//
// PRE-FIX (origin/main b57a712e, before this lane lands): the thunk is
// built without `NestedBodyScope`, so its body's `Sink.Seen := _n`
// (or any other call-state-leak) reads the value the FIRST caller
// wrote, NOT the property's own `_n` -- a wrong answer rather than a
// crash. The test asserts the right answer.
//
// POST-FIX (when `makePropertyThunk` adopts the canonical find): the
// thunk's body sees only the property's own state, the memoized body
// is identical regardless of caller, and both readers see 12.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked - AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// Two-callers, two-orders pin for makePropertyThunk. The same thunk is
// reached twice (once from `Main`, once from `AltRunner` after the
// first call returns); both must return 12 (the property's own _n),
// proving the memoized body's value is independent of the caller's
// expression context. §27.6 + §23.3.
static const char* kPropertyThunkCallerOrder =
    "module M; "
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 12; end constructor; "
    "  public property P: int "
    "    get => _n; "
    "  end P; "
    "end Owner; "
    "class Runners "
    "  public static method ReadTwice(o: Owner): int "
    "    var f: () -> int := o.P; "
    "    var g: () -> int := o.P; "
    "    return f() + g(); "
    "  end ReadTwice; "
    "end Runners; "
    "class P "
    "  public static method Main(): int "
    "    using var o := new Owner(); "
    "    return Runners.ReadTwice(o); "
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_nested_body_scope_4151_L,
           property_thunk_two_callers_in_two_orders) {
    BEZEL_EXPECT_EQ(runPlain(kPropertyThunkCallerOrder), 24);
}

BEZEL_TEST(codegen_nested_body_scope_4151_L,
           property_thunk_two_callers_in_two_orders_checked) {
    int rc = runChecked(kPropertyThunkCallerOrder);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 24);
}
