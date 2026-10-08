// Proposal 0082 §3 — the scoped await-bind, stage-1 PROTOTYPE (issue #3048).
//
// WHAT IS UNDER TEST. Spec §30.5 (v0.247) "Consumer-side rules", Pattern A:
//
//   "A borrow that would **escape** the full expression of an unbound-temporary
//    await — a `var` binding, a store, a non-chained return — has no source that
//    survives the expression, and the await is a compile error (K8.4: rejected,
//    never lifetime-extended); bind the task to a `using var` first."
//
// so `var cfg := await FetchAsync();` is an error at HEAD, and the spec's own
// fix is the two-line form (§30.5's example):
//
//     using var task := FetchAsync(url);
//     var response := await task;              // borrowed reference, OK
//
// Proposal 0082 §3 asks the compiler to write that pair itself for exactly this
// shape, via a hidden §28.10 `using var` slot, and states the desugaring is
// LITERAL: `var x := await E;` ⇒ `using var $t := E; var x := await $t;`. Its
// adoption gate C3 asks whether that hidden binding is the §28.10 cleanup-stack
// slot AND NOTHING ELSE.
//
// THE TESTS ARE THE C3 EVIDENCE, and they are written as an EQUIVALENCE:
// for each observable the proposal names in §3 (scope-end disposal point,
// reverse establishment order against neighbouring `using var`s, per-iteration
// disposal in a loop body, two slots in one scope, the exception edge, the
// escaping-borrow rejection), the one-line form compiled WITH the prototype
// flag must produce byte-identical behaviour to the hand-written two-line form
// compiled WITHOUT it. Where the two agree on every one of those, the hidden
// binding IS the ordinary `using var` and no new machinery was needed.
//
// FLAG-OFF IS ALSO UNDER TEST. The prototype is off by default
// (`CodegenOptions::scopedAwaitBind`, CLI `--experimental-scoped-await-bind`).
// Every "…_flag_off_…" case below pins the unchanged §30.5 behaviour: the bare
// bind is still rejected with the §30.5 message, and every already-legal shape
// still means exactly what it meant.
//
// FAIL-WITHOUT / PASS-WITH. Reverting src/driver/ScopedAwaitBind.cpp (or the
// two-line hook in Driver.cpp) makes every `…_flag_on_…` case fail: the program
// is rejected by §30.5 and `ran` is false. The `…_flag_off_…` cases pass either
// way — they are the "the default compiler did not move" half of the evidence.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked (the strict oracle)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::codegen::CodegenOptions;

namespace {

// The prototype's one bit (#3048). `proto(false)` is HEAD's compiler exactly.
CodegenOptions proto(bool on) {
    CodegenOptions o;
    o.scopedAwaitBind = on;
    return o;
}

// Exit-code channel (0..255).
int run(const std::string& prog, bool flagOn) {
    auto r = runAot(prog, proto(flagOn));
    return r.ran ? r.exitCode : -999;
}

// stdout channel, trailing line-ending stripped — for observables outside
// 0..255 (the disposal-order digit strings below).
std::string out(const std::string& prog, bool flagOn) {
    auto r = runAot(prog, proto(flagOn));
    if (!r.ran) return "<did-not-run>";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// True when the compile was rejected with §30.5's Pattern A consumer-side
// message — the diagnostic the prototype's trigger keys on.
bool rejectedByPatternA(const std::string& prog, bool flagOn) {
    auto r = runAot(prog, proto(flagOn));
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("(Pattern A) requires") != std::string::npos)
            return true;
    return false;
}

// `Cfg` — a plain class result, so `Task<Cfg>` is Pattern A (§30.5 producer-
// side rules: "`Task<T>` for **class** T").
const char* kCfg =
    "class Cfg public v: int; "
    "  public constructor(x: int) v := x; end constructor; "
    "end Cfg; ";

// `Cell` — the live-instance counter (ctor ++, dtor --), the leak/double-free
// oracle established by tests/codegen/async_task_result_disposal_1503_test.cpp.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// `T` — a disposal-ORDER tracer: each destructor appends its id to a decimal
// log, so the log read left-to-right is the disposal sequence.
const char* kTracer =
    "class Log public static s: int := 0; end Log; "
    "class T public v: int; "
    "  public constructor(i: int) v := i; end constructor; "
    "  destructor Log.s := Log.s * 10 + v; end destructor; "
    "end T; ";

}  // namespace

// ---------------------------------------------------------------------------
// 1. The motivating form. §30.5 rejects it today; proposal 0082 §0/§3 admits it.
// ---------------------------------------------------------------------------

static const std::string kBareBindProg =
    std::string("module M; ") + kCfg +
    "class C "
    "  public static async method FetchAsync(): Task<Cfg> "
    "    return new Cfg(42); "
    "  end FetchAsync; "
    "  public static async method Main(): Task<int> "
    "    var cfg := await C.FetchAsync(); "        // §30.5 error at HEAD
    "    return cfg.v; "
    "  end Main; "
    "end C;";

// FLAG OFF — unchanged §30.5: "A bare `var x := await someTask;` where
// `someTask` is not in one of the admitted positions is a compile error."
BEZEL_TEST(scoped_await_bind_proto_3048, bare_bind_flag_off_rejected_30_5) {
    BEZEL_EXPECT_TRUE(rejectedByPatternA(kBareBindProg, /*flagOn=*/false));
}

// FLAG ON — proposal §3: admitted, and `cfg` borrows the task's result.
BEZEL_TEST(scoped_await_bind_proto_3048, bare_bind_flag_on_admitted_0082_3) {
    BEZEL_EXPECT_EQ(run(kBareBindProg, /*flagOn=*/true), 42);
}

// ---------------------------------------------------------------------------
// 2. The disposal POINT (proposal §3: "disposed at the end of the innermost
//    enclosing block"; §28.10: "`using var` declares a local that is disposed
//    when the enclosing block ends"). The task's result is alive inside the
//    block and gone after it — §30.5 "disposing the task disposes the result".
// ---------------------------------------------------------------------------

static const std::string kScopeEndProg =
    std::string("module M; ") + kCell +
    "class C "
    "  public static async method LoadAsync(): Task<Cell> "
    "    return new Cell(); "
    "  end LoadAsync; "
    "  public static async method Main(): Task<int> "
    "    var inside := 0; "
    "    do "
    "      var c := await C.LoadAsync(); "
    "      inside := Cell.n; "                     // 1 — result alive here
    "    end; "
    "    if inside <> 1 then return 90 + inside; end; "
    "    return Cell.n; "                          // 0 — slot disposed at `end`
    "  end Main; "
    "end C;";

// The hand-written two-line form of the SAME program — the equivalence target.
static const std::string kScopeEndTwoLineProg =
    std::string("module M; ") + kCell +
    "class C "
    "  public static async method LoadAsync(): Task<Cell> "
    "    return new Cell(); "
    "  end LoadAsync; "
    "  public static async method Main(): Task<int> "
    "    var inside := 0; "
    "    do "
    "      using var task := C.LoadAsync(); "
    "      var c := await task; "
    "      inside := Cell.n; "
    "    end; "
    "    if inside <> 1 then return 90 + inside; end; "
    "    return Cell.n; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, scope_end_disposal_flag_on_28_10) {
    BEZEL_EXPECT_EQ(run(kScopeEndProg, /*flagOn=*/true), 0);
}

// The two-line form is the ORACLE: it must give the same answer at HEAD, with
// the flag off. If this ever diverges, the equivalence above is meaningless.
BEZEL_TEST(scoped_await_bind_proto_3048,
           scope_end_disposal_two_line_form_flag_off_28_10) {
    BEZEL_EXPECT_EQ(run(kScopeEndTwoLineProg, /*flagOn=*/false), 0);
}

// The same program, under generated-code ASan + LeakSanitizer: the hidden slot
// disposes the task and its result exactly once (proposal §9 "LSan-clean under
// the AOT harness").
BEZEL_TEST(scoped_await_bind_proto_3048, scope_end_disposal_flag_on_lsan_clean) {
    auto r = runAotChecked(kScopeEndProg, proto(/*flagOn=*/true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// 3. Disposal ORDER. §28.10: "pending cleanups run in reverse order of
//    establishment"; proposal §3: "a `using var` declared *after* the bind
//    disposes *before* the slot; one declared *before* disposes after —
//    identical to the two-line form."
//
//    Layout:   using var a := new T(1);
//              <the bind, producing T(2) inside the task>
//              using var b := new T(3);
//    Reverse establishment order  =>  3, then 2, then 1  =>  the log reads 321.
// ---------------------------------------------------------------------------

static const std::string kOrderScopedProg =
    std::string("module M; ") + kTracer +
    "class C "
    "  public static async method LoadAsync(): Task<T> "
    "    return new T(2); "
    "  end LoadAsync; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      using var a := new T(1); "
    "      var c := await C.LoadAsync(); "
    "      using var b := new T(3); "
    "    end; "
    "    Console.WriteLine(Log.s); "
    "    return 0; "
    "  end Main; "
    "end C;";

static const std::string kOrderTwoLineProg =
    std::string("module M; ") + kTracer +
    "class C "
    "  public static async method LoadAsync(): Task<T> "
    "    return new T(2); "
    "  end LoadAsync; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      using var a := new T(1); "
    "      using var task := C.LoadAsync(); "
    "      var c := await task; "
    "      using var b := new T(3); "
    "    end; "
    "    Console.WriteLine(Log.s); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The two-line form's order at HEAD — the oracle this equivalence is measured
// against, recorded explicitly rather than assumed.
BEZEL_TEST(scoped_await_bind_proto_3048,
           reverse_establishment_order_two_line_form_flag_off_28_10) {
    BEZEL_EXPECT_EQ(out(kOrderTwoLineProg, /*flagOn=*/false), std::string("321"));
}

// The scoped bind must land on the SAME cleanup stack, at the SAME position.
BEZEL_TEST(scoped_await_bind_proto_3048,
           reverse_establishment_order_flag_on_matches_two_line_28_10) {
    BEZEL_EXPECT_EQ(out(kOrderScopedProg, /*flagOn=*/true), std::string("321"));
    BEZEL_EXPECT_EQ(out(kOrderScopedProg, /*flagOn=*/true),
                    out(kOrderTwoLineProg, /*flagOn=*/false));
}

// ---------------------------------------------------------------------------
// 4. Two scoped binds in one scope — proposal §3: "creates two slots, disposed
//    in reverse order at scope end: byte-for-byte what the two hand-written
//    pairs produce." Disposal order 2 then 1 => the log reads 21.
// ---------------------------------------------------------------------------

static const std::string kTwoBindsProg =
    std::string("module M; ") + kTracer +
    "class C "
    "  public static async method MakeAsync(i: int): Task<T> "
    "    return new T(i); "
    "  end MakeAsync; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      var p := await C.MakeAsync(1); "
    "      var q := await C.MakeAsync(2); "
    "    end; "
    "    Console.WriteLine(Log.s); "
    "    return 0; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, two_binds_reverse_order_flag_on_28_10) {
    BEZEL_EXPECT_EQ(out(kTwoBindsProg, /*flagOn=*/true), std::string("21"));
}

// ---------------------------------------------------------------------------
// 5. The EXCEPTION edge. §28.10: "Disposal happens on every exit path: normal
//    completion, `break`, `return`, or exception." Proposal §3: "If a *later*
//    statement throws, the slot unwinds in stack order. No new edge."
// ---------------------------------------------------------------------------

static const std::string kThrowAfterBindProg =
    std::string("module M; ") + kCell +
    "class C "
    "  public static async method LoadAsync(): Task<Cell> "
    "    return new Cell(); "
    "  end LoadAsync; "
    "  public static method Boom(): int throw new Exception(\"boom\"); end Boom; "
    "  public static async method Main(): Task<int> "
    "    var caught := 0; "
    "    try "
    "      var c := await C.LoadAsync(); "
    "      caught := C.Boom(); "                    // throws AFTER the slot exists
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 90; end; "
    "    return Cell.n; "                           // 0 — slot unwound
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, exception_edge_disposes_slot_flag_on_28_10) {
    BEZEL_EXPECT_EQ(run(kThrowAfterBindProg, /*flagOn=*/true), 0);
}

BEZEL_TEST(scoped_await_bind_proto_3048,
           exception_edge_disposes_slot_flag_on_lsan_clean) {
    auto r = runAotChecked(kThrowAfterBindProg, proto(/*flagOn=*/true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// 6. A bind in a LOOP BODY pins to the loop *body* — proposal §3: "per-
//    iteration disposal, no accumulation across iterations".
// ---------------------------------------------------------------------------

static const std::string kLoopBodyProg =
    std::string("module M; ") + kCell +
    "class C "
    "  public static async method LoadAsync(): Task<Cell> "
    "    return new Cell(); "
    "  end LoadAsync; "
    "  public static async method Main(): Task<int> "
    "    var maxSeen := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var c := await C.LoadAsync(); "
    "      if Cell.n > maxSeen then maxSeen := Cell.n; end; "
    "      i := i + 1; "
    "    end; "
    "    if Cell.n <> 0 then return 90; end; "      // nothing accumulated
    "    return maxSeen; "                          // 1 — one live Cell per pass
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, loop_body_per_iteration_disposal_flag_on) {
    BEZEL_EXPECT_EQ(run(kLoopBodyProg, /*flagOn=*/true), 1);
}

// ---------------------------------------------------------------------------
// 7. `x` is an ORDINARY BORROWED LOCAL — proposal §2: "every §28.3 borrow rule
//    applies to `x` unchanged". §30.5's may-not-flow list therefore governs it
//    verbatim: "The borrow may **not** flow to … an `owned` parameter
//    argument." The desugaring must buy a LIFETIME, never OWNERSHIP, so the
//    scoped form must be rejected with the SAME diagnostic as the hand-written
//    two-line form — no new consume-site machinery, no new admission.
// ---------------------------------------------------------------------------

static const std::string kOwnedSinkScopedProg =
    std::string("module M; ") + kCfg +
    "class C "
    "  public static async method FetchAsync(): Task<Cfg> "
    "    return new Cfg(42); "
    "  end FetchAsync; "
    "  public static method Sink(owned c: Cfg): int "
    "    var r := c.v; c.Dispose(); return r; "
    "  end Sink; "
    "  public static async method Main(): Task<int> "
    "    var cfg := await C.FetchAsync(); "
    "    return C.Sink(move cfg); "                // borrow -> owned sink
    "  end Main; "
    "end C;";

static const std::string kOwnedSinkTwoLineProg =
    std::string("module M; ") + kCfg +
    "class C "
    "  public static async method FetchAsync(): Task<Cfg> "
    "    return new Cfg(42); "
    "  end FetchAsync; "
    "  public static method Sink(owned c: Cfg): int "
    "    var r := c.v; c.Dispose(); return r; "
    "  end Sink; "
    "  public static async method Main(): Task<int> "
    "    using var task := C.FetchAsync(); "
    "    var cfg := await task; "
    "    return C.Sink(move cfg); "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048,
           borrow_to_owned_sink_rejected_flag_on_matches_two_line_30_5) {
    auto scoped  = runAot(kOwnedSinkScopedProg,  proto(/*flagOn=*/true));
    auto twoLine = runAot(kOwnedSinkTwoLineProg, proto(/*flagOn=*/false));
    BEZEL_EXPECT_FALSE(twoLine.ran);
    BEZEL_EXPECT_FALSE(scoped.ran);
    // The SAME diagnostic, not merely the same verdict: the desugared `cfg` is
    // an ordinary borrow to every rule that inspects it.
    const char* kBorrowToOwned =
        "a borrow cannot be passed where an `owned` value is required";
    bool twoLineMsg = false, scopedMsg = false;
    for (const auto& d : twoLine.diagnostics.diagnostics())
        if (d.message.find(kBorrowToOwned) != std::string::npos) twoLineMsg = true;
    for (const auto& d : scoped.diagnostics.diagnostics())
        if (d.message.find(kBorrowToOwned) != std::string::npos) scopedMsg = true;
    BEZEL_EXPECT_TRUE(twoLineMsg);
    BEZEL_EXPECT_TRUE(scopedMsg);
}

// ---------------------------------------------------------------------------
// 8. The boundaries proposal §2 keeps REJECTED, verified under the flag.
// ---------------------------------------------------------------------------

// `using var x := await E;` — "the await produces a borrow and `using var`
// requires an owned RHS (§30.5's flow-to list)". Still rejected with the flag.
static const std::string kUsingVarBindProg =
    std::string("module M; ") + kCfg +
    "class C "
    "  public static async method FetchAsync(): Task<Cfg> "
    "    return new Cfg(42); "
    "  end FetchAsync; "
    "  public static async method Main(): Task<int> "
    "    using var cfg := await C.FetchAsync(); "
    "    return cfg.v; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, using_var_bind_still_rejected_flag_on_30_5) {
    auto r = runAot(kUsingVarBindProg, proto(/*flagOn=*/true));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Assignment to an EXISTING local — "stays rejected in v1" (proposal §2).
static const std::string kAssignExistingProg =
    std::string("module M; ") + kCfg +
    "class C "
    "  public static async method FetchAsync(): Task<Cfg> "
    "    return new Cfg(42); "
    "  end FetchAsync; "
    "  public static async method Main(): Task<int> "
    "    using var task := C.FetchAsync(); "
    "    var cfg := await task; "
    "    cfg := await C.FetchAsync(); "             // still an error
    "    return cfg.v; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048,
           assignment_to_existing_local_still_rejected_flag_on_30_5) {
    BEZEL_EXPECT_TRUE(rejectedByPatternA(kAssignExistingProg, /*flagOn=*/true));
}

// An if-EXPRESSION initializer whose ARMS are awaits — proposal §1/§3: "the
// initializer must be *exactly* the await", and §5's containment claim depends
// on it ("each widening grows the invisible lifetime past 'the two-liner you
// would have written here'"). The ownership pass passes `bindingValue` THROUGH
// a ConditionalExpr, so both arms are reported; neither is the whole
// initializer, so neither is rewritten and the program stays rejected.
static const std::string kIfExprArmsProg =
    std::string("module M; ") + kCfg +
    "class C "
    "  public static async method FetchAsync(): Task<Cfg> "
    "    return new Cfg(42); "
    "  end FetchAsync; "
    "  public static async method FetchAltAsync(): Task<Cfg> "
    "    return new Cfg(7); "
    "  end FetchAltAsync; "
    "  public static async method Main(): Task<int> "
    "    var useAlt := false; "
    "    var cfg := (if useAlt then await C.FetchAltAsync() "
    "                          else await C.FetchAsync()); "
    "    return cfg.v; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048,
           if_expression_arms_still_rejected_flag_on_0082_1) {
    BEZEL_EXPECT_TRUE(rejectedByPatternA(kIfExprArmsProg, /*flagOn=*/false));
    BEZEL_EXPECT_TRUE(rejectedByPatternA(kIfExprArmsProg, /*flagOn=*/true));
}

// ---------------------------------------------------------------------------
// 9. Non-firing: shapes that already compile must mean exactly what they meant.
//    Proposal §9: "a **pure widening**; the rule fires only on today's errors,
//    so no existing program changes meaning or timing."
// ---------------------------------------------------------------------------

// A NAMED operand — §30.5's canonical form. The rule "does not engage".
static const std::string kNamedOperandProg =
    std::string("module M; ") + kCfg +
    "class C "
    "  public static async method FetchAsync(): Task<Cfg> "
    "    return new Cfg(42); "
    "  end FetchAsync; "
    "  public static async method Main(): Task<int> "
    "    using var task := C.FetchAsync(); "
    "    var cfg := await task; "
    "    return cfg.v; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, named_operand_unchanged_flag_off_30_5) {
    BEZEL_EXPECT_EQ(run(kNamedOperandProg, /*flagOn=*/false), 42);
}
BEZEL_TEST(scoped_await_bind_proto_3048, named_operand_unchanged_flag_on_30_5) {
    BEZEL_EXPECT_EQ(run(kNamedOperandProg, /*flagOn=*/true), 42);
}

// A COMPOSITION — `(await E).v`. Proposal §1 keeps it OUT of the rule ("the
// initializer must be *exactly* the await"), and §30.5 already admits it: the
// borrow is confined to the same full expression. Only the ACCEPTANCE VERDICT
// is asserted here, not the value: at HEAD the confined unbound-temporary form
// reads freed memory (issue #3070 — codegen frees the anonymous task AT the
// await instead of at the K8 full-expression boundary), so the value is
// nondeterministic garbage with or without this flag. What the prototype owes
// is that it does not change the verdict, and that is what is checked.
static const std::string kCompositionProg =
    std::string("module M; ") + kCfg +
    "class C "
    "  public static async method FetchAsync(): Task<Cfg> "
    "    return new Cfg(42); "
    "  end FetchAsync; "
    "  public static async method Main(): Task<int> "
    "    var n := (await C.FetchAsync()).v; "
    "    return n; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, composition_not_rejected_flag_off_28_3) {
    BEZEL_EXPECT_FALSE(rejectedByPatternA(kCompositionProg, /*flagOn=*/false));
}
BEZEL_TEST(scoped_await_bind_proto_3048, composition_not_rejected_flag_on_28_3) {
    BEZEL_EXPECT_FALSE(rejectedByPatternA(kCompositionProg, /*flagOn=*/true));
}

// PATTERN B is untouched — proposal §9: "`var x := await AcquireAsync();` stays
// an owned bind" (§30.5: "`x` is an owned local under the standard §28.3
// discharge rules").
static const std::string kPatternBProg =
    "module M; "
    "class Conn public v: int; "
    "  public constructor(x: int) v := x; end constructor; "
    "end Conn; "
    "class C "
    "  public static async method AcquireAsync(): OwningTask<Conn> "
    "    return new Conn(7); "
    "  end AcquireAsync; "
    "  public static async method Main(): Task<int> "
    "    var c := await C.AcquireAsync(); "         // owned bind, not a borrow
    "    var r := c.v; "
    "    c.Dispose(); "
    "    return r; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, pattern_b_bind_unchanged_flag_off_30_5) {
    BEZEL_EXPECT_EQ(run(kPatternBProg, /*flagOn=*/false), 7);
}
BEZEL_TEST(scoped_await_bind_proto_3048, pattern_b_bind_unchanged_flag_on_30_5) {
    BEZEL_EXPECT_EQ(run(kPatternBProg, /*flagOn=*/true), 7);
}

// A VALUE-typed result (`Task<int>`) — §30.5's no-obligation row: the bare
// producer call is already admitted and the await consumes the temporary at
// the end of the full expression. The prototype must not touch it (its site
// list can only contain Pattern A rejections).
static const std::string kValueResultProg =
    "module M; "
    "class C "
    "  public static async method CountAsync(): Task<int> "
    "    return 42; "
    "  end CountAsync; "
    "  public static async method Main(): Task<int> "
    "    var n := await C.CountAsync(); "
    "    return n; "
    "  end Main; "
    "end C;";

BEZEL_TEST(scoped_await_bind_proto_3048, value_result_bind_unchanged_flag_off_30_5) {
    BEZEL_EXPECT_EQ(run(kValueResultProg, /*flagOn=*/false), 42);
}
BEZEL_TEST(scoped_await_bind_proto_3048, value_result_bind_unchanged_flag_on_30_5) {
    BEZEL_EXPECT_EQ(run(kValueResultProg, /*flagOn=*/true), 42);
}
