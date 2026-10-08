// Spec-derived tests for issue #404 — a SOUNDNESS hole #392 left open: a
// catch-bound exception variable could be SUNK into an `owned`-consuming
// position (an `owned` parameter, an owning-collection `set(owned)` indexer),
// retaining the in-flight exception past the arm.
//
// §31.1 "Exception ownership": "Inside a catch arm, the bound exception
// variable (`var ex: Exception` in the arm pattern) is a **borrow** of the
// in-flight exception; its source is the runtime, and its lifetime is the arm
// body's. The borrow may be passed to other methods (which further borrow for
// the duration of the call) but may not be assigned to an `owned` field,
// returned as `owned`, or captured by a closure that may outlive the arm."
// "Exception instances themselves cannot escape the arm that caught them."
//
// §28.8 (ownership transfer): "When a parameter is `owned`, the caller
// relinquishes ownership and may not use the argument after the call." Passing
// the catch binding to an `owned` parameter (or an owning-collection
// `set(owned)` slot, §28.11) is therefore a TRANSFER — the sink takes
// responsibility for disposing the value and may retain it past the call. For
// the caught exception that is exactly the escape §31.1 forbids: the runtime
// disposes the exception after the arm completes, so the retained reference
// dangles (a double-dispose / use-after-free).
//
// #392 already rejects store-to-owned-field, return-as-owned, and
// escaping-closure-capture; #395/#398/#402 reject a borrow flowing into an
// `owned` parameter / owned-field-read sink / owned-field assignment for owned
// LOCALS and PARAMETERS. But the catch binding is neither a tracked local nor a
// parameter, so the unified borrow classifier `ownedArgSourceIsBorrow` fell
// through to its conservative "cannot classify → not a borrow" tail and
// PERMITTED the sink. #404 teaches that classifier to recognize the catch
// binding as a borrow, which closes the escape across every owned-consuming
// position at once.
//
// SOUNDNESS DISCIPLINE: every positive rejects a program §31.1 makes an error;
// every negative is a structurally-similar but spec-LEGAL program that must be
// accepted. Over-rejection is the cardinal sin — §31.1 EXPLICITLY permits
// passing the binding to a method as a borrow ("may be passed to other methods
// ... for the duration of the call"), and copying value/string data out of it
// ("copy the relevant data out of it ... into separate owned values"). The
// negatives pin those boundaries.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A `Holder` modeling the owned-parameter / owning-slot sinks for the caught
// exception. Built-in `Exception` (§31.4) is a class, so `owned Exception?`
// fields and `owned e: Exception` parameters are well-formed (the existing
// catch_escape_test uses an `owned Exception?` field the same way).
//
//  - `_e: owned Exception?`        — the owned-field DESTINATION (§28.7/§28.8).
//  - `Set(owned e: Exception)`     — the owned-PARAMETER sink (the headline
//                                    #404 case): it moves `e` into `_e`.
//  - `set(owned)` indexer          — the owning-collection slot sink (§28.11).
//  - `Helper(e: Exception)`        — the BORROW-parameter control: §31.1
//                                    explicitly allows passing the binding here.
//  - `_text: string?`             — a non-owned field for the `ex.Message`
//                                    value-projection escape hatch.
//  - `DoWork()`                    — the protected operation in the `try` body.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class Holder "
        "  private owned _e: Exception?; "
        "  private _text: string?; "
        "  public method Set(owned e: Exception) "
        "    this._e := e; "
        "  end Set; "
        "  public property [i: int]: Exception "
        "    get => this._e!; "
        "    set(owned) => do "
        "      using var hold := move value; "
        "    end; "
        "  end; ") + method +
        "  private method Helper(e: Exception) end Helper; "
        "  private method DoWork() end DoWork; "
        "end Holder;";
}

} // namespace

// =====================================================================
// NEGATIVES (must be REJECTED) — the owned-consuming sinks #404 closes.
// =====================================================================

// §31.1 / §28.8 — the headline #404 case: passing the catch binding to an
// `owned` parameter. `Set(owned e)` moves the caught exception into `_e`,
// retaining it past the arm. Before #404 this compiled (the classifier could
// not see the catch binding as a borrow). REJECTED.
BEZEL_TEST(catch_owned_sink, pass_binding_to_owned_param_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => this.Set(ex); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "a borrow cannot be passed where an `owned` value is required"));
}

// §31.1 / §28.8 — same with an explicit `owned` call-site modifier on the
// binding. Still the in-flight exception, still a borrow, still REJECTED.
BEZEL_TEST(catch_owned_sink, pass_binding_to_owned_param_explicit_modifier_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => this.Set(move ex); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "a borrow cannot be passed where an `owned` value is required"));
}

// §31.1 / §28.8 — `move ex` into the owned parameter. `move` of a borrow is no
// more an owned value than the bare binding (the binding is owned by the
// runtime, not this scope). REJECTED.
BEZEL_TEST(catch_owned_sink, move_binding_to_owned_param_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => this.Set(move ex); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "a borrow cannot be passed where an `owned` value is required"));
}

// §31.1 / §28.8 — indirect via a re-borrow holder: `var b := ex; this.Set(b);`.
// The holder `b` re-borrows the in-flight exception (Borrowed state, source
// `ex`); sinking it into the owned parameter is the same retention. REJECTED.
BEZEL_TEST(catch_owned_sink, pass_reborrow_holder_to_owned_param_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => do "
        "    var b := ex; "
        "    this.Set(b); "
        "  end; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "a borrow cannot be passed where an `owned` value is required"));
}

// §31.1 / §28.11 — the owning-collection `set(owned)` indexer: `s[0] := ex;`
// requires an owned RHS; the slot would dispose the value it stores. Handing
// the caught exception's borrow to the slot retains it past the arm. REJECTED.
// (The receiver is a `Holder` PARAMETER `s`, matching the existing
// owned_sink_test convention `s[0] := …`. A `this`-receiver indexer setter is
// not yet recognized by the owned-slot check — a separate, pre-existing gap
// unrelated to the catch-borrow classification — so the parameter-receiver form
// is the faithful exercise of the §28.11 `set(owned)` slot sink.)
BEZEL_TEST(catch_owned_sink, pass_binding_to_set_owned_indexer_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(s: Holder) "
        "  try DoWork(); "
        "  catch var ex: Exception => s[0] := ex; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "a borrow cannot be passed where an `owned` value is required"));
}

// §31.1 / §28.3 / §28.5 — assigning the catch binding to an `owned` field
// directly: `this._e := ex;`. (This path was already reached by #392's
// store-to-field machinery; included so the #404 classifier change is verified
// not to regress it.) REJECTED.
BEZEL_TEST(catch_owned_sink, assign_binding_to_owned_field_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => this._e := ex; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `ex` may not be assigned to a field"));
}

// §31.1 / §28.8 — passing the catch binding as an `owned` CONSTRUCTOR argument
// is the same retention hazard as an `owned` method parameter. REJECTED.
BEZEL_TEST(catch_owned_sink, pass_binding_to_owned_ctor_arg_rejected_31_1) {
    auto r = parseAndCheckOwnership(std::string(
        "module M; "
        "class Box "
        "  private owned _e: Exception?; "
        "  public constructor(owned e: Exception) "
        "    this._e := e; "
        "  end constructor; "
        "end Box; "
        "class Holder "
        "  private method DoWork() end DoWork; "
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => using var box := new Box(ex); "
        "    end; "
        "  end Handle; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "this call consumes `ex`") ||
        hasErrorMessage(r, "scope-bound"));
}

// =====================================================================
// POSITIVES (must be ACCEPTED) — the §31.1-legal shapes. Zero over-rejection.
// =====================================================================

// §31.1 — passing the binding to a BORROW parameter is EXPLICITLY allowed:
// "may be passed to other methods (which further borrow for the duration of the
// call)". `Helper(e: Exception)` does not consume ownership. ACCEPTED.
BEZEL_TEST(catch_owned_sink, pass_binding_to_borrow_param_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => this.Helper(ex); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 — copying a VALUE/string projection out of the exception into an owned
// position is the documented escape hatch ("copy the relevant data out of it,
// typically `ex.Message` ... into separate owned values"). `ex.Message` is a
// string copy, not the instance, so storing it into the (non-owned) string
// field is legal. ACCEPTED.
BEZEL_TEST(catch_owned_sink, project_message_into_field_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => this._text := ex.Message; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 / §31.2.4 — the bare re-throw `throw;` transfers the in-flight
// exception back to the runtime rather than capturing it; "the one exception to
// this rule". ACCEPTED.
BEZEL_TEST(catch_owned_sink, bare_rethrow_clean_31_2_4) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => throw; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 / §17.12 (v0.182, #1569) — re-raising the SAME instance with
// `throw ex;` is a compile error: the caught binding is a borrow and the throw
// operand must be OWNED. Use the bare re-raise `throw;` instead. REJECTED.
BEZEL_TEST(catch_owned_sink, rethrow_borrowed_binding_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => throw ex; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "the thrown value must be an OWNED exception"));
}

// §31.2.4 — wrapping: `throw new InvalidOperationException();` raises a fresh
// owned instance; the catch binding is not sunk anywhere. ACCEPTED.
BEZEL_TEST(catch_owned_sink, wrap_and_throw_new_clean_31_2_4) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => throw new InvalidOperationException(); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 — the idiomatic shape: pass the binding as a borrow to a helper AND
// copy a value projection out of it, then let it go. Nothing escapes or is
// sunk. ACCEPTED.
BEZEL_TEST(catch_owned_sink, borrow_call_and_project_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => do "
        "    this.Helper(ex); "
        "    this._text := ex.Message; "
        "  end; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 — REGRESSION GUARD: a genuine owned value flowing into the SAME
// `Set(owned e)` sink, from OUTSIDE any catch arm, must stay accepted. The
// #404 classifier change must only recognize the catch binding as a borrow, not
// disturb the legal `new`-expr transfer into an owned parameter. ACCEPTED.
BEZEL_TEST(catch_owned_sink, owned_new_into_owned_param_outside_catch_clean_28_8) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Feed() "
        "  this.Set(new Exception()); "
        "end Feed;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
