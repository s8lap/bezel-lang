// Spec-derived regression test for issue #2795.
//
// §23.4 (Indexers) / §7.1 (Non-nullable and nullable types) / §7.4 / §23
// (Properties and indexers), v0.240.
//
// §23.4 states, verbatim:
//
//     "Inside a member of a class with its own indexer, `this[i]` is required
//      (the brackets need a left-hand side)."
//
// so `this[i]` is the spelling the spec MANDATES, not an exotic one. But the
// `IdentifierExpr` arm of `ExprTC::visitExpr` deliberately leaves `this`
// WITHOUT an `exprType` (only #456's primitive/builtin-`extension` case sets
// one), and the whole `IndexExpr` typing block was guarded by
// `if (auto* tt = ctx_.exprType(*ix->target))`. For a `this`-rooted index that
// guard was false, so the ENTIRE block was skipped and everything it does was
// missing on exactly the spelling §23.4 requires:
//
//   * the indexer RESULT TYPE (§23.4). `var s := this[i].ToString();` produced
//     "internal: lowerLocalDecl's initializer expression failed to lower to a
//     value ... (degraded to a zero-initialized value) - #2434" and malformed
//     IR ("Function return type does not match operand type of return inst!
//     ret i32 %4 / ptr"). A bare `return this[i];` happened to work because the
//     return coercion supplies the type — which is why this went unnoticed.
//   * the #2530 `PropertySymbol` attachment (drives the ref/out operand gate
//     and codegen's #2540 string-release relaxation — see the LEAK case below).
//   * the #2543 write-only-indexer read gate (§23).
//   * the #2619 §7 index-argument check. `this[NI()]` with an un-narrowed
//     `int?` index compiled with NO diagnostic and returned the WRONG VALUE
//     (the null index lowered to 0). A silent wrong value is strictly worse
//     than the malformed IR every other receiver family produced.
//
// The fix resolves the receiver type LOCALLY inside the `IndexExpr` arm
// (src/sema/ExprTypecheck_Exprs.cpp) instead of publishing an `exprType` for
// `this` globally: the wide shape would turn on every `exprType(this)` reader
// at once, including codegen's `indexReceiverDecl`, whose #747 R42 branch
// deliberately recovers the owner TypeSymbol from `methodFns_` /
// `currentSpecMethodSymbol_`.
//
// This is BOTH a value fix and a rejection fix, so:
//   * every value case asserts CORRECT VALUES (not merely a clean compile);
//   * every rejection case asserts on diagnostic TEXT, never the coarse
//     `rejects()` helper (runAot's WIFSIGNALED path synthesizes an Error
//     diagnostic for a runtime SIGSEGV too);
//   * the accept controls are the load-bearing half — a widened rejection that
//     breaks working code is worse than the bug it fixes.
//
// NOT covered, deliberately:
//   * `base[i]` (#2832) — codegen's `indexReceiverDecl` resolves `base` to the
//     DERIVED type, so `base[i]` already dispatches to the override (a
//     confirmed silent wrong value: 202 where 102 is expected, while `base.P`
//     and `base.M()` correctly bypass). Typing it here would only widen the set
//     of spellings that reach that miscompile.
//   * `this[i]` inside a LAMBDA body (#2833) — codegen cannot lower it (the
//     `methodFns_` reverse lookup fails for a closure's llvm::Function), so it
//     yields 0 both before and after this fix (measured identical).
//   * indexer overload selection ignoring nullability (#2834) — why the
//     nullable-declared-parameter accept control below uses a class whose ONLY
//     indexer is `[n: int?]`: with an `[i: int]` sibling present the resolver
//     picks the non-nullable one for an `int?` argument, on a variable receiver
//     too.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §7 "nullable -> non-nullable" assignability diagnostic fragment, shared
// with #2619's own gates.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

}  // namespace

// ---------------------------------------------------------------------------
// SYMPTOM B (the worst one) — an un-narrowed nullable index through `this`.
// Pre-fix: compiled with NO diagnostic and printed 1 (the null `int?` became 0
// and the getter returned 0 + 1).
// ---------------------------------------------------------------------------

// #2795 symptom B / §7.1 + §23.4: the issue's exact repro.
BEZEL_TEST(sema_this_indexer_typing_2795,
           this_index_rejects_unnarrowed_nullable_index_7_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Self "
        "  private static method NI(): int? => null; "
        "  public property [i: int]: int => i + 1; "
        "  public method Bad(): int return this[NI()]; end Bad; "
        "end Self; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Self[i]`"));
    BEZEL_EXPECT_TRUE(hasError(r, "(\xC2\xA7""7)"));
}

// #2795 / §7.1 + §23.4 — the same hole on a MULTI-parameter indexer through
// `this`: the ordinal subject wording must name which argument is at fault.
BEZEL_TEST(sema_this_indexer_typing_2795,
           this_multi_index_rejects_unnarrowed_nullable_index_7_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Grid "
        "  private static method NI(): int? => null; "
        "  public property [r: int, c: int]: int => r * 100 + c; "
        "  public method Bad(): int return this[1, NI()]; end Bad; "
        "end Grid; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument 2 to the `Grid` indexer"));
}

// ---------------------------------------------------------------------------
// SYMPTOM A — the result type. VALUES, not a clean compile.
// ---------------------------------------------------------------------------

// #2795 symptom A / §23.4 — `this[i]` in every position that needs the
// EXPRESSION's own type: an arithmetic operand, a call argument, a binary
// operand, a `case` scrutinee, and a `:=` assignment target read back through
// the getter.
//
// The sweep is run TWICE, on an `int`-returning and a `string`-returning
// indexer. The `int` half is an accept control that passes either way — a
// value-typed slot happens to survive because codegen's zero-init fallback
// lowers to the same `i32` the position wants, which is exactly why the bug
// stayed hidden. The `string` half is what OBSERVES the defect: every one of
// these positions degraded pre-fix (measured: the empty string / 0), and the
// program as a whole failed IR verification with "Function return type does not
// match operand type of return inst! ret i32 %4 / ptr".
BEZEL_TEST(sema_this_indexer_typing_2795,
           this_index_result_type_drives_every_value_position_23_4) {
    auto r = runAot(
        "module M; "
        "class SGrid "
        "  public property [i: int]: string => i.ToString(); "
        "  public static method Len(s: string): int => s.Length; "
        "  public method L(i: int): int => this[i].Length; "
        "  public method Cat(i: int): int => (this[i] + \"!\").Length; "
        "  public method AsArg(i: int): int => SGrid.Len(this[i]); "
        "  public method Scrut(i: int): int "
        "    return (case this[i] of \"0\" => 1, \"12\" => 2, _ => 3); "
        "  end Scrut; "
        "end SGrid; "
        "class Grid "
        "  public property [i: int]: int => i * 10 + 3; "
        "  public static method Twice(v: int): int => v * 2; "
        "  public method Arith(i: int): int => this[i] + 1; "
        "  public method AsArg(i: int): int => Grid.Twice(this[i]); "
        "  public method Cmp(i: int): bool => this[i] > Grid.Twice(i); "
        "  public method Scrut(i: int): int "
        "    return (case this[i] of 3 => 4, 13 => 6, _ => 5); "
        "  end Scrut; "
        "end Grid; "
        "class Store "
        "  private owned _a: Array<int>; "
        "  public constructor() _a := new Array<int>(4, 0); end constructor; "
        "  public property [i: int]: int "
        "    get => _a[i]; "
        "    set => do _a[i] := value; end; "
        "  end; "
        "  public method Bump(i: int): int "
        "    this[i] := this[i] + 7; "
        "    return this[i]; "
        "  end Bump; "
        "end Store; "
        "class C public static method Main(): int "
        "  using var g := new Grid(); "
        "  using var s := new Store(); "
        "  using var q := new SGrid(); "
        "  var t := 0; "
        // the `int` half — accept control (passes with and without the fix)
        "  t := t + g.Arith(1); "                       // 13 + 1     -> 14
        "  t := t + g.AsArg(1); "                       // Twice(13)  -> 40
        "  if g.Cmp(1) then t := t + 1; end; "          // 13 > 2     -> 41
        "  t := t + g.Scrut(0); "                       // this[0]=3  -> 45
        "  t := t + g.Scrut(1); "                       // this[1]=13 -> 51
        "  t := t + g.Scrut(2); "                       // this[2]=23 -> 56
        "  t := t + s.Bump(0); "                        // 0 + 7      -> 63
        "  t := t + s.Bump(1); "                        // 0 + 7      -> 70
        // the `string` half — this is what fails without the fix
        "  t := t + q.L(12); "                          // \"12\".Length -> 72
        "  t := t + q.Cat(12); "                        // \"12!\"       -> 75
        "  t := t + q.AsArg(12); "                      // Len(\"12\")   -> 77
        "  t := t + q.Scrut(0); "                       // \"0\"  -> 1   -> 78
        "  t := t + q.Scrut(12); "                      // \"12\" -> 2   -> 80
        "  t := t + q.Scrut(5); "                       // _    -> 3   -> 83
        "  return t; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 83);
}

// #2795 symptom A / §23.4 — the STRING-typed observations the issue reports:
// `this[i].ToString()` (int indexer), `this[i].Length` (string indexer), and
// an interpolation hole. Pre-fix these were the sites that emitted
// "Function return type does not match operand type of return inst!" and the
// #2434 zero-degradation.
BEZEL_TEST(sema_this_indexer_typing_2795,
           this_index_result_type_drives_member_access_on_result_23_4) {
    BEZEL_EXPECT_TRUE(runAotOut(
        "module M; "
        "class Self "
        "  public property [i: int]: int => i + 10; "
        "  public method B(i: int): string => this[i].ToString(); "
        "end Self; "
        "class C public static method Main(): int "
        "  using var s := new Self(); "
        "  Console.WriteLine(s.B(5)); "
        "  return 0; "
        "end Main; end C;") == "15");

    // A string-typed indexer: `this[i].Length` needs the result to be `string`.
    BEZEL_EXPECT_TRUE(runAotOut(
        "module M; "
        "class Str "
        "  public property [i: int]: string => \"abc\"; "
        "  public method L(i: int): int => this[i].Length; "
        "end Str; "
        "class C public static method Main(): int "
        "  using var t := new Str(); "
        "  Console.WriteLine(t.L(0)); "
        "  return 0; "
        "end Main; end C;") == "3");

    // A string-KEYED indexer overload selected through `this`.
    BEZEL_EXPECT_TRUE(runAotOut(
        "module M; "
        "class Grid "
        "  public property [i: int]: int => i + 1; "
        "  public property [k: string]: int => k.Length; "
        "  public method KeyLen(k: string): string => this[k].ToString(); "
        "end Grid; "
        "class C public static method Main(): int "
        "  using var g := new Grid(); "
        "  Console.WriteLine(g.KeyLen(\"abcd\")); "
        "  return 0; "
        "end Main; end C;") == "4");
}

// ---------------------------------------------------------------------------
// The receiver GRID: record, interface-declared, inherited, and generic.
// ---------------------------------------------------------------------------

// #2795 / §23.4 ("Records may declare read-only indexers"; "Indexers may be
// declared in interfaces") + §12 (inheritance) — `this[i]`'s type must resolve
// for a record receiver, a class implementing an interface-declared indexer,
// and an INHERITED indexer used through `this` in the derived class.
BEZEL_TEST(sema_this_indexer_typing_2795,
           this_index_types_across_record_interface_and_inherited_23_4) {
    BEZEL_EXPECT_TRUE(runAotOut(
        "module M; "
        "record RIdx "
        "  public W: int; "
        "  public property [i: int]: int => i * W; "
        "  public method Str(i: int): string => this[i].ToString(); "
        "end RIdx; "
        "interface IGrid readonly property [i: int]: int; end IGrid; "
        "class GridImpl: IGrid "
        "  public property [i: int]: int => i + 40; "
        "  public method Str(i: int): string => this[i].ToString(); "
        "end GridImpl; "
        "class BaseIdx public property [i: int]: int => i + 70; end BaseIdx; "
        "class DerivedIdx: BaseIdx "
        "  public method Str(i: int): string => this[i].ToString(); "
        "end DerivedIdx; "
        "class C public static method Main(): int "
        "  var r := new RIdx(6); "
        "  using var gi := new GridImpl(); "
        "  using var d := new DerivedIdx(); "
        "  Console.WriteLine(r.Str(2) + \"|\" + gi.Str(1) + \"|\" + d.Str(1)); "
        "  return 0; "
        "end Main; end C;") == "12|41|71");
}

// #2795 / §14.1 + §14.2 + §23.4 — the GENERIC axis, the one a fix that is
// right for a non-generic class can silently get wrong. The receiver TypeRef
// synthesized for `this` carries NO generic arguments, so
// `substituteMemberType` resolves the indexer's declared result type through
// its own `this`-target branch (#747 R42, seeded from `currentType_`):
//   * inside the DECLARING generic class, `this[i]` stays the class's own
//     rigid parameter (an `int`-returning indexer on `Holder<T>` must still be
//     `int`, and must not be confused with T);
//   * through an INHERITED generic-base indexer, `T` must resolve to the
//     enclosing instantiation's binding (`VGDerived: VGBase<Pt>` -> `Pt`), so
//     `this[i].X` resolves the record's field.
BEZEL_TEST(sema_this_indexer_typing_2795,
           this_index_substitutes_against_the_enclosing_instantiation_14_1) {
    auto r = runAot(
        "module M; "
        "record Pt public X: int; public Y: int; end Pt; "
        "class Holder<T> "
        "  public property [i: int]: int => i + 5; "
        "  public method N(i: int): int => this[i].ToString().Length; "
        "end Holder; "
        "class VGBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "end VGBase; "
        "class VGDerived: VGBase<Pt> "
        "  public constructor(p: Pt) base(p); end constructor; "
        "  public method XOf(i: int): int => this[i].X; "
        "  public method YOf(i: int): int => this[i].Y; "
        "end VGDerived; "
        "class C public static method Main(): int "
        "  using var h := new Holder<string>(); "
        "  using var vg := new VGDerived(new Pt(33, 44)); "
        "  var t := 0; "
        "  t := t + h.N(2); "        // this[2] = 7 -> \"7\".Length = 1
        "  t := t + h.N(95); "       // this[95] = 100 -> \"100\".Length = 3 -> 4
        "  t := t + vg.XOf(0); "     // 37
        "  t := t + vg.YOf(0); "     // 81
        "  return t; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 81);
}

// ---------------------------------------------------------------------------
// ACCEPT CONTROLS — the load-bearing half. Every legal `this[…]` index
// spelling must still compile AND run.
// ---------------------------------------------------------------------------

// #2795 / §7.4 ("narrowing"), §7.7 (`??`), §7.8 (`x!`), §23.4 — the accept
// controls #2619 shipped for a variable receiver, re-spelled through `this`
// (the receiver family its check could not reach). §7.4 narrowing deliberately
// does NOT rewrite `exprType`, so a naive "is the argument type nullable"
// rejection would break every narrowed form here.
BEZEL_TEST(sema_this_indexer_typing_2795,
           legal_this_index_spellings_are_not_rejected_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "enum E A, B; end E; "
        "class Acc "
        "  private const K: int := 2; "
        "  public property [i: int]: int => i + 1; "
        "  public property [c: E]: int => 50; "
        "  public property [r: int, s: int]: int => r * 100 + s; "
        "  private static method SI(): int? => 7; "
        "  public method A1(): int => this[3]; "
        "  public method A2(): int "
        "    var n := Acc.SI(); if n <> null then return this[n]; end; "
        "    return -1; end A2; "
        "  public method A3(): int => this[Acc.SI()!]; "
        "  public method A4(): int => this[Acc.SI() ?? 0]; "
        "  public method A5(): int => this[E.A]; "
        "  public method A6(): int => this[Acc.K]; "
        "  public method A7(): int => this[2, 5]; "
        "  public method A8(): int "
        "    var n := Acc.SI(); if n = null then return -1; end; "
        "    return this[n]; end A8; "
        "  public method A9(): int var i := 4; return this[i]; end A9; "
        "end Acc; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
}

// #2795 / §7.1 gate (4) — the one shape where a nullable index through `this`
// is LEGAL: the declared index parameter is itself `int?`. Read through
// `substituteMemberType` against the receiver, so it must work for a `this`
// receiver exactly as for a variable one.
BEZEL_TEST(sema_this_indexer_typing_2795,
           nullable_declared_index_parameter_still_accepts_nullable_through_this) {
    auto r = parseSemaAll(
        "module M; "
        "class NAcc "
        "  private static method NI(): int? => null; "
        "  public property [n: int?]: string => \"ok\"; "
        "  public method A(): string => this[NAcc.NI()]; "
        "end NAcc; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
}

// #2795 END-TO-END accept control: sema acceptance is not enough — the accepted
// spellings must still COMPILE AND RUN with the right values.
BEZEL_TEST(sema_this_indexer_typing_2795,
           legal_this_index_spellings_still_compile_and_run) {
    auto r = runAot(
        "module M; "
        "enum E A, B; end E; "
        "class Acc "
        "  private const K: int := 2; "
        "  public property [i: int]: int => i + 1; "
        "  public property [c: E]: int => 50; "
        "  public property [r: int, s: int]: int => r + s; "
        "  private static method SI(): int? => 7; "
        "  public method Total(): int "
        "    var t := 0; "
        "    t := t + this[3]; "                                   // 4
        "    var n := Acc.SI(); "
        "    if n <> null then t := t + this[n]; end; "            // 12
        "    t := t + this[Acc.SI()!]; "                           // 20
        "    t := t + this[Acc.SI() ?? 0]; "                       // 28
        "    t := t + this[E.A]; "                                 // 78
        "    t := t + this[Acc.K]; "                               // 81
        "    t := t + this[2, 5]; "                                // 88
        "    var i := 4; t := t + this[i]; "                       // 93
        "    return t; "
        "  end Total; "
        "end Acc; "
        "class NAcc "
        "  private static method NI(): int? => null; "
        "  public property [n: int?]: int => 9; "
        "  public method A(): int => this[NAcc.NI()]; "
        "end NAcc; "
        "class C public static method Main(): int "
        "  using var a := new Acc(); "
        "  using var n := new NAcc(); "
        "  return a.Total() + n.A(); "                             // 93 + 9
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 102);
}

// ---------------------------------------------------------------------------
// The #2543 write-only gate, now reachable through `this`.
// ---------------------------------------------------------------------------

// #2795 + #2543 / §23 — a bare READ of a write-only indexer through `this` is
// rejected (the gate lives in the block that was skipped for a `this`
// receiver). Pre-fix this fell through to codegen's #2434 zero-degradation.
BEZEL_TEST(sema_this_indexer_typing_2795,
           read_of_write_only_indexer_through_this_is_rejected_23) {
    auto r = parseSemaAll(
        "module M; "
        "class W "
        "  private owned _a: Array<int>; "
        "  public constructor() _a := new Array<int>(4, 0); end constructor; "
        "  public property [i: int]: int "
        "    set => do _a[i] := value; end; "
        "  end; "
        "  public method BadRead(i: int): int => this[i]; "
        "end W; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "a read of a write-only indexer is not "
                                  "allowed"));
    BEZEL_EXPECT_TRUE(hasError(r, "the `W` indexer is write-only"));
}

// #2795 + #2543 / §23 — the accept half of the same gate: a plain `:=`
// assignment through `this` dispatches to the SETTER and is the legal escape
// hatch, so it must NOT be rejected, and must still write the slot.
BEZEL_TEST(sema_this_indexer_typing_2795,
           write_only_indexer_set_through_this_is_accepted_23) {
    auto r = runAot(
        "module M; "
        "class W "
        "  private owned _a: Array<int>; "
        "  public constructor() _a := new Array<int>(4, 0); end constructor; "
        "  public property [i: int]: int "
        "    set => do _a[i] := value; end; "
        "  end; "
        "  public method Store(i: int, v: int) this[i] := v; end Store; "
        "  public method Peek(i: int): int => _a[i]; "
        "end W; "
        "class C public static method Main(): int "
        "  using var w := new W(); "
        "  w.Store(0, 5); "
        "  w.Store(1, 9); "
        "  return w.Peek(0) + w.Peek(1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 14);
}

// ---------------------------------------------------------------------------
// The static-member control and the memory net.
// ---------------------------------------------------------------------------

// #2795 / §12.3 — `this` is not in scope in a static method. The fix must NOT
// type `this[i]` there: the existing §12.3 rejection stays the diagnostic, with
// no §7 or §23 cascade added on top of it.
BEZEL_TEST(sema_this_indexer_typing_2795,
           this_index_in_a_static_method_keeps_its_existing_rejection_12_3) {
    auto r = parseSemaAll(
        "module M; "
        "class S "
        "  public property [i: int]: int => i + 1; "
        "  public static method Bad(i: int): int return this[i]; end Bad; "
        "end S; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(
        r, "`this` is not available in a static method or static constructor"));
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!hasError(r, "a read of a write-only indexer"));
}

// #2795 + #2540 / §28.9 + §23.4 — the LEAK the missing #2530 PropertySymbol
// attachment caused. Codegen's string-local refcount analysis identifies an
// indexer-SET through `ctx.exprSymbol(*ix)`; with no symbol attached for a
// `this` receiver, `this[i] := <string local>` fell through to the conservative
// ESCAPED taint, suppressing the local's scope-end release and leaking its +1
// per iteration. Measured on the pre-fix compiler with this exact program:
// "SUMMARY: AddressSanitizer: 350 byte(s) leaked in 20 allocation(s)."
BEZEL_TEST(sema_this_indexer_typing_2795,
           string_indexer_set_through_this_is_leak_free_28_9) {
    auto r = runAotChecked(
        "module M; "
        "class SBox "
        "  private owned _a: Array<string>; "
        "  public constructor() "
        "    _a := new Array<string>(2, \"\"); "
        "  end constructor; "
        "  public property [i: int]: string "
        "    get => _a[i]; "
        "    set => do _a[i] := value; end; "
        "  end; "
        "  public method Fill(n: int) "
        "    var k := 0; "
        "    while k < n do "
        "      var s := \"v\" + k.ToString(); "
        "      this[k mod 2] := s; "
        "      k := k + 1; "
        "    end; "
        "  end Fill; "
        "  public method Len(i: int): int => this[i].Length; "
        "end SBox; "
        "class C public static method Main(): int "
        "  using var b := new SBox(); "
        "  b.Fill(10); "
        "  return b.Len(0) + b.Len(1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 4);
}
