// fix #4013 (§12.8 / §22.7 / §22.8, v0.294): a record constructor's leading
// `this(...)` chain call ran through `buildRecordValue`'s inline
// local-construction path (LlvmBackend_Exprs2.cpp), which — unlike every
// OTHER constructor-body lowering path (`emitConstructorBody`'s compiled
// ctor functions and the class-side `inlineCtorBody`, both of which use the
// shared `chainCallOf`/`emitChainCall` pair documented on
// `ast::chainCallOfStmt`, Node.hpp) — never recognised the chain call at
// all. It handed the WHOLE constructor body, chain statement included, to
// `lowerScopedBlock`. `this(...)`'s callee is the bare identifier `this`
// (see `chainCallOfStmt`), which names no ordinary callable symbol, so the
// general call-lowering machinery silently produced no IR for it and the
// chained-to constructor's body never ran — at ANY severity, no diagnostic.
// A field assigned only by the chained-to constructor stayed at its §7.9
// zero default; for a reference-typed field that is an unset pointer read
// back through a method call, which SIGSEGVs.
//
// fix #4019 (§22.8 "Field initializers run exactly once per instance
// construction", v0.294): making the chain call above actually run exposed
// a SECOND, compounding defect — `buildRecordValue`'s preamble runs every
// body field/auto-property initializer UNCONDITIONALLY, before it even
// looks at which constructor was selected (fix #2464). That was safe only
// because a chain call previously did nothing; once #4013 routes the chain
// through `emitChainCall`, the chain TARGET's own compiled ctor function
// (built by the shared `emitConstructorBody`) ALSO runs the initializers,
// via its own `!isThis`-gated `emitOwnFieldInit` — the exact double-run
// §22.8 names by name ("would run every field initializer twice under
// `this(...)` chaining — repeating allocations and initializer side
// effects, assigning `readonly` fields twice"). Fixed by resolving the
// selected constructor BEFORE the preamble runs and skipping the preamble's
// initializer loop for a `this(...)`-chaining constructor, letting the
// existing once-only authority (the same `!isThis` gate the class side
// already relies on) run it exactly once at the bottom of the chain.
//
// Every test here is value-asserting on `runAot`'s exit code — "it
// compiled" proves nothing for a silently-skipped chain call, and a
// CONSTANT field initializer proves nothing for a doubled one (a repeated
// store of the same constant is indistinguishable from a single store) —
// every field-initializer test below uses a non-idempotent initializer
// (a static counter) so a double execution is observable as a wrong value.
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// Minimal repro from issue 4013: a record's zero-arg ctor chains via
// `this(42)` to the one-arg ctor, which is the ONLY assigner of `N`. Before
// the fix this read back the §7.9 zero default (0), not 42.
BEZEL_TEST(codegen_record_this_chain_4013, record_this_chain_runs_target_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record R4013A "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public constructor() this(42); end constructor; "
        "end R4013A; "
        "class Program4013A "
        "  public static method Main(): int "
        "    var c := new R4013A(); return c.N; "
        "  end Main; "
        "end Program4013A;"), 42);
}

// The identical shape on a CLASS is the control that shows this was
// record-specific: `emitConstructorBody`/`inlineCtorBody` already handled
// `this(...)` correctly, so this must keep returning 42 on both sides of
// the fix (it is not part of the FAIL-WITHOUT proof).
BEZEL_TEST(codegen_record_this_chain_4013, class_this_chain_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class C4013Ctrl "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public constructor() this(42); end constructor; "
        "  public method N(): int return _n; end N; "
        "end C4013Ctrl; "
        "class Program4013Ctrl "
        "  public static method Main(): int "
        "    var c := new C4013Ctrl(); return c.N(); "
        "  end Main; "
        "end Program4013Ctrl;"), 42);
}

// Second repro from issue 4013: the chained-to constructor is the ONLY
// assigner of a non-nullable REFERENCE-typed field (a record property
// holding a class instance). Before the fix this SIGSEGVed on `.Render()`
// because `Extra`'s backing storage was never written by the never-run
// chained-to constructor; DefiniteAssignment.cpp's `runRecordConstructor`
// correctly treats the `this(...)` chain as discharging the §7.9
// obligation, so no compile-time diagnostic caught it either.
//
// fix #4095 (§9.7 / §22.7 / §22.8, v0.295): the same shape is now REJECTED at
// sema time. §9.7's no-temporary-source rule (already enforced for the OUTER
// call `new Bag([10, 20, 30])` where `Bag` borrows the literal) applies one
// level deeper, to a record-constructor's `this(new T(...))` chain whose
// target stores the producer into a borrow-carrying field. The fresh
// producer's +1 dies at the end of the chain call, while the record still
// carries a pointer to it — a dangling borrow (§9.7). The remedy names the
// source first: bind it to a local and pass the local. The accept-arm test
// `record_ctor_this_observation_gate_4014_test.cpp::this_chaining_ctor_body_
// call_accepted_spec_22_7` exercises the same record-shape chain but with the
// producer pre-bound to a local, and must keep passing on both sides. //
// per §9.7, §22.7, §22.8, v0.295
BEZEL_TEST(codegen_record_this_chain_4013,
           record_this_chain_discharges_reference_field) {
    auto r = runAot(
        "module M; "
        "public class Widget4013 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "  public method Render(): int return Count; end Render; "
        "end Widget4013; "
        "public record R4013B "
        "  public readonly property Extra: Widget4013; "
        "  public constructor(w: Widget4013) Extra := w; end constructor; "
        "  public constructor() this(new Widget4013(88)); end constructor; "
        "end R4013B; "
        "class Program4013B "
        "  public static method Main(): int "
        "    var c := new R4013B(); return c.Extra.Render(); "
        "  end Main; "
        "end Program4013B;");
    // §9.7 reject — the chain arg is a fresh owned producer into a non-`owned`
    // chain-target formal; the record's borrow-carrying field would dangle.
    // (LANE_RULES §3 + ambiguous-ran-false ratchet: pin the diagnostic by
    // message text + real location, not just `hasErrors()` / `r.ran`.)
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "fresh producer"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "this(...)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "section 9.7"));
}

// A two-hop record chain: ctor() -> this(42) -> this(42, 7) -> the ctor
// that actually stores both fields. Every intermediate hop must actually
// run for the value to come out right.
BEZEL_TEST(codegen_record_this_chain_4013, record_two_hop_chain_runs_every_hop) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record R4013C "
        "  public readonly property N: int; "
        "  public readonly property Q: int; "
        "  public constructor(n: int, q: int) N := n; Q := q; end constructor; "
        "  public constructor(n: int) this(n, 7); end constructor; "
        "  public constructor() this(42); end constructor; "
        "end R4013C; "
        "class Program4013C "
        "  public static method Main(): int "
        "    var c := new R4013C(); return c.N + c.Q; "
        "  end Main; "
        "end Program4013C;"), 49);  // N=42, Q=7
}

// fix #4019: a record `this(...)` chain composed with a body field
// initializer whose value is NOT idempotent (a static counter, incremented
// on every call) — a constant initializer cannot distinguish one execution
// from two, so this is the test that actually pins the once-only rule.
// Before the #4019 fix: the preamble ran `Counter4019A.Next()` once, then
// `emitChainCall`'s target ctor ran it AGAIN via its own field-init step,
// so `V` read back 2, not 1.
BEZEL_TEST(codegen_record_this_chain_4013,
           record_this_chain_runs_field_initializer_exactly_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Counter4019A "
        "  public static Value: int := 0; "
        "  public static method Next(): int "
        "    Counter4019A.Value := Counter4019A.Value + 1; "
        "    return Counter4019A.Value; "
        "  end Next; "
        "end Counter4019A; "
        "public record R4019A "
        "  public property V: int := Counter4019A.Next(); "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public constructor() this(5); end constructor; "
        "end R4019A; "
        "class Program4019A "
        "  public static method Main(): int "
        "    var c := new R4019A(); return c.V * 100 + c.N; "
        "  end Main; "
        "end Program4019A;"), 105);  // V=1 (not 2), N=5
}

// fix #4019: the identical shape, but the doubly-initialized field is
// `readonly` — §22.8 calls out "assigning `readonly` fields twice" as one
// of the once-only rule's named consequences (distinct from just repeating
// a side effect). The counter still proves it: a double execution would
// both increment the counter twice AND store to the readonly field twice.
BEZEL_TEST(codegen_record_this_chain_4013,
           record_this_chain_readonly_field_initializer_runs_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Counter4019B "
        "  public static Value: int := 0; "
        "  public static method Next(): int "
        "    Counter4019B.Value := Counter4019B.Value + 1; "
        "    return Counter4019B.Value; "
        "  end Next; "
        "end Counter4019B; "
        "public record R4019B "
        "  public readonly property Tag: int := Counter4019B.Next(); "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public constructor() this(7); end constructor; "
        "end R4019B; "
        "class Program4019B "
        "  public static method Main(): int "
        "    var c := new R4019B(); return c.Tag * 100 + c.N; "
        "  end Main; "
        "end Program4019B;"), 107);  // Tag=1 (not 2), N=7
}

// fix #4019: the identical defect reached through a GENERIC record's
// `this(...)` chain, where the chain target may be reached via
// `inlineCtorBody` (per-instantiation inlining) rather than a standalone
// compiled ctor function — `inlineCtorBody` carries the SAME `!isThis`
// field-init gate, so it is not narrow to the non-generic path tested
// above.
//
// `Item` is deliberately NOT the observable here: the chain target's
// argument is `default(T)`, and for `T = int` that is `0` — indistinguishable
// from "the chain never ran at all" (issue 4013's own defect), which would
// ALSO leave `Item` at its `0` watermark. A second counter (`Marker4019D`),
// incremented only inside the chain TARGET's own body, is what actually
// tells apart all three states: never ran (Marked stays 0), ran but with
// the field initializer doubled (Tag == 2), and correct (Tag == 1, Marked
// == 1).
BEZEL_TEST(codegen_record_this_chain_4013,
           record_this_chain_generic_field_initializer_runs_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Counter4019C "
        "  public static Value: int := 0; "
        "  public static method Next(): int "
        "    Counter4019C.Value := Counter4019C.Value + 1; "
        "    return Counter4019C.Value; "
        "  end Next; "
        "end Counter4019C; "
        "public class Marker4019D "
        "  public static Ran: int := 0; "
        "  public static method Mark(): int "
        "    Marker4019D.Ran := Marker4019D.Ran + 1; "
        "    return Marker4019D.Ran; "
        "  end Mark; "
        "end Marker4019D; "
        "public record GBox4019<T> "
        "  public property Tag: int := Counter4019C.Next(); "
        "  public readonly property Item: T; "
        "  public readonly property Marked: int; "
        "  public constructor(item: T) "
        "    Item := item; "
        "    Marked := Marker4019D.Mark(); "
        "  end constructor; "
        "  public constructor() this(default(T)); end constructor; "
        "end GBox4019; "
        "class Program4019C "
        "  public static method Main(): int "
        "    var b := new GBox4019<int>(); return b.Tag * 100 + b.Marked; "
        "  end Main; "
        "end Program4019C;"), 101);  // Tag=1 (not 2), Marked=1 (not 0)
}

