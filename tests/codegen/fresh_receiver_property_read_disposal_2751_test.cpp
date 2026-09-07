// Codegen — issue #2751: `(new T()).Member` never disposes the temporary when
// `Member` is a PROPERTY. §28.3 "Unbound owned temporaries" (spec v0.240).
//
// THE SPEC. §28.3 enumerates the member read among the borrowing uses that
// produce an unbound owned temporary:
//   "An owned value may also be produced as an **unbound temporary**: the
//    result of an `owned`-returning call (or other owned producer) that is not
//    bound to any name, not `move`d, and not returned, but used only by
//    *borrowing* operations within the expression — as the receiver of a
//    further call, as a non-`owned` argument, as a member or index access, or
//    as the scrutinee of a `case` … Such a temporary is disposed at the **end
//    of the full expression** in which it is produced, after every use of it
//    within that expression has completed"
// and, in the same section's recap of the discard contract:
//   "an *unnamed* owned temporary — which no named form can reach — is
//    reclaimed automatically once the expression that produced it is finished
//    with it."
//
// THE DEFECT. #1039 implemented that reclaim for exactly two member-read
// shapes: the FIELD read (`fieldAddr` path in `lowerExprInner`) and the METHOD
// call (`emitMethodCall`). A PROPERTY read is neither — it resolves to a
// `PropertySymbol` and is lowered by one of four getter paths in
// `LlvmBackend_Exprs.cpp`, none of which disposed the receiver:
//   * `lowerVirtualPropertyGet`       — virtual / interface dispatch
//   * the inline computed-getter path — `get => expr` (also extension props)
//   * the block-getter path           — `get => do … end`
//   * the imported-getter path        — a cross-module property
// So `(new Src()).N` leaked the `Src` in every position tested: the ordinary
// local-variable initializer, the constructor field-initializer, and the bare
// expression statement. The AUTO property (`property N: int := 5;`) was the
// only form already covered, because it is field-backed and travels #1039's
// field path — which is why the shape looked handled.
//
// Reproduced pre-fix on the issue's own repro under `bezelc --sanitize=address`
// (20-iteration loop, ctor +1 / dtor -1 live counter): arrow-bodied getter,
// block-bodied getter and virtual getter each reported 320 bytes leaked in 20
// allocations with the live counter at 20; the auto-property control was clean.
//
// THE FIX AND ITS DELIBERATE LIMIT. Each of the four paths now calls the
// existing `disposeFreshNewReceiverTemp`, which is the sound subset gate: it
// fires only when the receiver is a fresh owned-class producer (`new T()` or an
// `owned`-returning call) AND the extracted result is a NON-REFERENCE value
// (numeric/bool/char/decimal primitive, enum, unit). `calleeReturnsOwned` is
// FALSE at every property site by construction — §A.3's property grammar
// carries no `owned` marker on a property type — so a property read can never
// announce a fresh transfer and the #1859/#2205 class/interface/owning-
// collection widenings stay unreachable here. That keeps the fix clear of
// #2261's use-after-free shape: a property returning a `string`, a class
// reference, a record, `Object`, or an iterable/`Task` interface is NOT
// disposed and keeps leaking — the sound direction, asserted by the EXCLUSION
// cases below (a bounded leak is strictly better than a use-after-free).
//
// THE ORACLE IS A LIVE-OBJECT COUNTER, NOT MERELY "NO LEAK". Every case folds
// BOTH halves into the exit code as `n * 10 + made` — how many instances are
// still alive, and how many were ever constructed. A leak shows up as `n > 0`
// and a double dispose as a wrong `made` / a crash, so the disposed-EXACTLY-
// once claim is observable as a value and does not rest on a sanitizer verdict.
// (A one-iteration `Main` is an unreliable LSan oracle — a leftover register
// copy makes LSan call a leaked block reachable — so every program loops; the
// sanitizer leg is the secondary net.)

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked / …Lenient
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain (non-instrumented) AOT run — the exit code alone, or -999 when the
// compile or the run did not complete (a SIGSEGV is WIFSIGNALED -> ran=false).
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan + LSan run. A leak / double-free / use-after-free surfaces as ran=false
// with the sanitizer report in `diagnostics`, i.e. -999.
int runChecked(const std::string& src) {
    JitResult r = bezel::test::runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

// ASan WITHOUT LeakSanitizer — for the EXCLUSION cases, whose receivers are
// deliberately not reclaimed. Use-after-free / double-free detection in the
// generated code is unchanged, which is exactly the load-bearing assertion
// there ("the excluded shapes must never free a still-aliased receiver").
int runCheckedLenient(const std::string& src) {
    JitResult r = bezel::test::runAotCheckedLenient(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

// Both oracles agree on `expected`; a skipped ASan leg is tolerated.
void expectBoth(const std::string& src, int expected) {
    BEZEL_EXPECT_EQ(runPlain(src), expected);
    int c = runChecked(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, expected);
}

void expectBothLenient(const std::string& src, int expected) {
    BEZEL_EXPECT_EQ(runPlain(src), expected);
    int c = runCheckedLenient(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, expected);
}

// ---------------------------------------------------------------------------
// Program builder. `Src.n` is the live count (ctor +1 / dtor -1), `Src.made`
// the construction count; `Main` returns `Src.n * 10 + Src.made` after running
// `body` eight times.
//   one temporary per iteration, all reclaimed  ->  0 * 10 +  8 ==  8
//   one temporary per iteration, all leaked     ->  8 * 10 +  8 == 88
// `extra` holds any auxiliary declarations; module-level declaration order is
// immaterial (§11), so it sits after `Src`.
// ---------------------------------------------------------------------------
std::string prog(const std::string& members, const std::string& body,
                 const std::string& extra = "") {
    return
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class Src "
        "  public static n: int := 0; "
        "  public static made: int := 0; "
        "  private _v: int := 5; "
        "  public constructor() Src.n := Src.n + 1; "
        "    Src.made := Src.made + 1; end constructor; "
        "  destructor Src.n := Src.n - 1; end destructor; "
        + members +
        "end Src; " + extra +
        "class P "
        "  public static method Take(a: int, b: int): int return a + b; end Take; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 8 do " + body + " i := i + 1; end while; "
        "    if total < 0 then return 250; end if; "
        "    return Src.n * 10 + Src.made; "
        "  end Main; "
        "end P;";
}

// The three local getter body forms (§23.2 / §23.3 / §12.1).
const char* kArrowProp =
    "  public property N: int "
    "    get => _v; "
    "  end N; ";
const char* kBlockProp =
    "  public property N: int "
    "    get => do return _v; end; "
    "  end N; ";
const char* kVirtualProp =
    "  public virtual property N: int "
    "    get => _v; "
    "  end N; ";

// ---------------------------------------------------------------------------
// FAIL-WITHOUT / PASS-WITH: the three getter forms, the issue's repro shape.
// Pre-fix each of these returned 88 (eight leaked receivers) instead of 8.
// ---------------------------------------------------------------------------

// §23.2 expression-bodied getter — the issue's own smallest repro.
BEZEL_TEST(fresh_receiver_property_2751, arrow_getter_value_result_disposed) {
    expectBoth(prog(kArrowProp, "total := total + (new Src()).N; "), 8);
}

// §23.3 block-bodied getter (`get => do … end;`) — the issue's variant 3.
BEZEL_TEST(fresh_receiver_property_2751, block_getter_value_result_disposed) {
    expectBoth(prog(kBlockProp, "total := total + (new Src()).N; "), 8);
}

// §12.1 / §23: a VIRTUAL property dispatches through the vtable
// (lowerVirtualPropertyGet) — a distinct lowering path with the same gap.
BEZEL_TEST(fresh_receiver_property_2751, virtual_getter_value_result_disposed) {
    expectBoth(prog(kVirtualProp, "total := total + (new Src()).N; "), 8);
}

// CONTROL (already green pre-fix): an AUTO property is field-backed, so it
// travelled #1039's field path and was never part of the defect. Pinned so a
// future change to the field path cannot silently regress it.
BEZEL_TEST(fresh_receiver_property_2751, auto_property_control_disposed) {
    expectBoth(prog("  public property A: int := 5; ",
                    "total := total + (new Src()).A; "), 8);
}

// §28.3: the producer need not be a `new` — an `owned`-returning call is the
// other fresh-producer form the same rule governs ("the result of an
// `owned`-returning call (or other owned producer)").
BEZEL_TEST(fresh_receiver_property_2751, owned_producer_receiver_disposed) {
    expectBoth(prog(kArrowProp, "total := total + Mk.Make().N; ",
                    "class Mk public static method Make(): owned Src "
                    "  return new Src(); end Make; end Mk; "),
               8);
}

// The issue's SECOND repro: the same read in a constructor FIELD INITIALIZER.
// (The issue was first filed as field-initializer-specific, then corrected to
// cover both; both positions are pinned so neither can regress alone.)
BEZEL_TEST(fresh_receiver_property_2751, field_initializer_position_disposed) {
    expectBoth(prog(kArrowProp,
                    "using var o := new Owner(); total := total + o.Cache(); ",
                    "class Owner "
                    "  private _cache: int := (new Src()).N; "
                    "  public constructor() end constructor; "
                    "  public method Cache(): int return _cache; end Cache; "
                    "end Owner; "),
               8);
}

// §28.3's boundary is the FULL EXPRESSION, not just a `var` initializer: a bare
// expression statement, a binary operand, a call argument, a conditional arm
// and a `const` recipient are all the same shape. Seven temporaries per
// iteration (56 made, 0 live) — all must be reclaimed.
BEZEL_TEST(fresh_receiver_property_2751, nested_expression_positions_disposed) {
    expectBoth(prog("  public property N: int get => _v; end N; "
                    "  public property Q: bool get => _v > 0; end Q; ",
                    "const a := (new Src()).N; "
                    "(new Src()).N; "
                    "total := total + a "
                    "  + (new Src()).N + (new Src()).N "
                    "  + P.Take((new Src()).N, (new Src()).N) "
                    "  + (if (new Src()).Q then 1 else 0); "),
               56);
}

// The value-result subset the gate admits, one kind at a time: int, bool, char,
// decimal, enum, and the NULLABLE `int?` spelling (a `{i8, i32}` value, not a
// reference). Six temporaries per iteration (48 made, 0 live).
BEZEL_TEST(fresh_receiver_property_2751, every_value_result_kind_disposed) {
    expectBoth(prog("  public property I: int get => 1; end I; "
                    "  public property B: bool get => true; end B; "
                    "  public property C: char get => 'x'; end C; "
                    "  public property D: decimal get => 1.5m; end D; "
                    "  public property E: Color get => Color.Green; end E; "
                    "  public property NI: int? get => 9; end NI; ",
                    "if (new Src()).B then total := total + (new Src()).I; end if; "
                    "total := total + int((new Src()).C) + int((new Src()).D) "
                    "  + int((new Src()).E) + ((new Src()).NI ?? 0); "),
               48);
}

// §14.1: a generic class's property, both the `T`-typed and a concrete-typed
// one, under monomorphisation. Four iterations x two temporaries.
BEZEL_TEST(fresh_receiver_property_2751, generic_class_property_disposed) {
    std::string src =
        "module M; "
        "class Box<T> "
        "  public static n: int := 0; "
        "  public static made: int := 0; "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; Box<T>.n := Box<T>.n + 1; "
        "    Box<T>.made := Box<T>.made + 1; end constructor; "
        "  destructor Box<T>.n := Box<T>.n - 1; end destructor; "
        "  public property Val: T get => _v; end Val; "
        "  public property Tag: int get => 3; end Tag; "
        "end Box; "
        "class P public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 4 do "
        "    total := total + (new Box<int>(7)).Val + (new Box<int>(7)).Tag; "
        "    i := i + 1; "
        "  end while; "
        "  if total < 0 then return 250; end if; "
        "  return Box<int>.n * 10 + Box<int>.made; "
        "end Main; end P;";
    expectBoth(src, 8);
}

// ---------------------------------------------------------------------------
// CONTROL: a BOUND receiver must not be double-disposed by the new reclaim.
// This is the direction that matters most — the fresh-producer gate is what
// keeps `using var s := new Src(); s.N` on the slot path alone. A double free
// would be a crash (ran=false, -999) or a negative live count, never 8.
// ---------------------------------------------------------------------------

BEZEL_TEST(fresh_receiver_property_2751, bound_local_receiver_not_double_disposed) {
    expectBoth(prog("  public property N: int get => _v; end N; "
                    "  public virtual property VN: int get => _v + 1; end VN; "
                    "  public property BN: int get => do return _v + 2; end; end BN; ",
                    "using var s := new Src(); "
                    "total := total + s.N + s.VN + s.BN; "),
               8);
}

// ---------------------------------------------------------------------------
// EXCLUSIONS: a result that IS (or may embed) a borrow of the receiver must
// leave the receiver alone. These programs LEAK by design — eight live at exit
// — and the load-bearing assertion is that generated-code ASan reports no
// use-after-free / double-free, i.e. the run completes and returns 88.
// ---------------------------------------------------------------------------

// A `string` property result borrows the receiver's refcounted string (#1039's
// own exclusion, restated for the property path).
BEZEL_TEST(fresh_receiver_property_2751, string_result_receiver_not_disposed) {
    expectBothLenient(
        prog("  private _s: string := \"hello\"; "
             "  public property S: string get => _s; end S; ",
             "total := total + (new Src()).S.Length; "),
        88);
}

// A CLASS-reference property result is a borrow of a receiver sub-object.
BEZEL_TEST(fresh_receiver_property_2751, class_result_receiver_not_disposed) {
    expectBothLenient(
        prog("  public owned L: Leaf := new Leaf(); "
             "  public property Lf: Leaf get => L; end Lf; ",
             "total := total + (new Src()).Lf.V; ",
             "class Leaf public V: int := 7; end Leaf; "),
        88);
}

// A RECORD result is a by-value struct that may embed a borrow (here a string
// field); §28.3's reclaim must not fire.
//
// `S` carries a field initializer (fix #4001, landed in this same batch: the
// record no-ctor §7.9 screen now correctly flags a non-nullable-string body
// FIELD with no constructor and no initializer too — `string` is not
// defaultable, ExprTypecheck_Exprs.cpp's `defaultIllFormed` — since `Pair`'s
// implicit constructor would otherwise admit a construction that leaves `S`
// unset). Verified the initializer is overridden by the positional argument
// at the one `new Pair(3, "abc")` call site below, so this is orthogonal to
// what the test exercises (receiver disposal, not `Pair`'s own construction).
BEZEL_TEST(fresh_receiver_property_2751, record_result_receiver_not_disposed) {
    expectBothLenient(
        prog("  private _p: Pair := new Pair(3, \"abc\"); "
             "  public property Pr: Pair get => _p; end Pr; ",
             "total := total + (new Src()).Pr.A; ",
             "record Pair public A: int; public S: string := \"\"; end Pair; "),
        88);
}

// §31: the accessor throws.
//
// UPDATED BY #2816 (§28.3 *Unbound owned temporaries*, v0.240). When this case
// was written the reclaim was emitted behind the pending-exception check, so it
// ran on the NORMAL return path only and the temporary leaked on the unwind
// edge; this case pinned that leak (8 live) as the then-safe direction. §28.3
// requires the disposal on the exception edge too — "every already-produced,
// not-yet-consumed unbound owned temporary is disposed during unwinding by the
// same implicit `try`/`finally` mechanism as named locals" — so #2816 registers
// the temporary at its production as a null-guarded `unwindOnly` disposal and
// the expected live count is now 0, i.e. 8 (== 0 * 10 + 8 made). The
// double-dispose half of the assertion is unchanged and is the reason the case
// stays: `expectBoth` runs it under generated-code ASan+LSan, so an over-release
// would fault rather than read as a smaller count. The dedicated coverage for
// the edge lives in fresh_receiver_temp_unwind_disposal_2816_test.cpp.
BEZEL_TEST(fresh_receiver_property_2751, getter_throw_edge_no_double_dispose) {
    expectBoth(
        prog("  public property Boom: int "
             "    get => do throw new InvalidOperationException(\"no\"); end; "
             "  end Boom; ",
             "try total := total + (new Src()).Boom; "
             "catch var e: InvalidOperationException => total := total + 1; end; "),
        8);
}

}  // namespace
