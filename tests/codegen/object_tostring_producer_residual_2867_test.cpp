// Spec-derived regression test for issue #2867 — the #2846 residual.
//
// §8.5 (`ToString`) / §8.6 (Boxing) / §28.9 (Runtime-managed types) /
// §14.1 (generics) / §31.1 (Throwing), v0.240.
//
// §8.6 states, verbatim:
//
//     "When a value type is used where `Object` (or an interface implemented by
//      the value type) is expected, the value is **boxed**: a heap-allocated
//      wrapper holds the value. Boxed values are runtime-managed ([§28.9])."
//
// §28.9 leaves the mechanism open but not the obligation:
//
//     "The implementation of runtime-managed types' lifecycle (reference
//      counting, region allocation, or other) is not part of this
//      specification."
//
// The `string` a `ToString` produces is itself runtime-managed, so it must be
// reclaimed exactly once — neither zero times (a leak) nor twice (a
// use-after-free).
//
// WHAT #2846 LEFT. #2846 made `o.ToString()` a recognized `+1` producer
// (`isPlusOneStringProducer`, LlvmBackend_Setup.cpp) for THREE of the four
// receiver shapes `lowerObjectToString`'s call site accepts
// (LlvmBackend_Calls2.cpp, under `if (!ms)`): `Object`-typed (primitive and the
// imported Named spelling), interface-typed, and class-typed-with-no-override.
// It did NOT cover:
//
//   * a BUILTIN-EXCEPTION receiver (`isBuiltinExceptionTypeRef`) — left out
//     because #2846 had not audited what fills such a vtable's slot 3;
//   * a BARE GENERIC-PARAMETER receiver whose active monomorphization binds `T`
//     to a class or an interface — the `genericSubstSemaTy_` redirect at
//     LlvmBackend_Calls2.cpp, which #2475's generic arm explicitly excluded
//     ("that substitution dispatches through `lowerObjectToString`, which may
//     or may not allocate").
//
// Both kept the pre-#2846 behaviour: the consumer borrow-RETAINS and the
// producer's `+1` is never dropped — one leaked string per call, unbounded in a
// loop, whenever the dispatch target actually allocates.
//
// THE AUDIT #2867 ASKED FOR, and its answer. Both arms are `+1`-or-sentinel:
//   * a genuine built-in exception carries `bvt$<name>`, whose slot 3 is
//     `bz$extostr$<name>` — a thunk returning §8.5's "for classes: the type's
//     qualified name" as an immortal constant global (setupBuiltinExceptions,
//     LlvmBackend_Setup.cpp). Retain/release on it are no-ops;
//   * a USER class deriving from a built-in exception (§31.1 admits any class)
//     carries its OWN class vtable, so slot 3 is `classToStringThunk` — the
//     same filler the `isNamedClassRecv` arm #2846 already took relies on: the
//     user `override ToString()`, whose ReturnStmt lowering
//     (LlvmBackend_Stmts.cpp) mints `+1` for a `string` return — §28.3
//     classifies an announced return as a transfer to the caller — else the
//     class-name literal;
//   * a `T` bound to a class or interface reaches the identical vtable slot 3,
//     whose fillers are `classToStringThunk` or the box thunks
//     (`emitRecordToString` / `emitPrimitiveToString` / a literal).
//
// MEASURED before the fix, 20 iterations plus a churn allocation:
//   * builtin-exception receiver, minting override — 711 bytes / 40
//     allocations from `bz$App.MyEx$ToString` via `bz$tostr$MyEx`;
//   * bare-`T` receiver bound to a class with a minting override — 711 bytes /
//     40 allocations from `bz$App.K$ToString` via `bz$tostr$K`.
//
// EVIDENCE DISCIPLINE. Every leak case is LOOP-AMPLIFIED (20 iterations plus a
// churn allocation): a one-shot `Main` is not a reliable LSan oracle, because a
// leftover pointer in a register makes a leaked block read as merely reachable.
// Every string is built at RUNTIME (`"K" + n.ToString()`), never folded from
// two literals — a literal is an immortal sentinel on which release is a no-op,
// which would make the probe vacuous. And the direction a pure "no leak"
// assertion is BLIND to — OVER-release, which is where this family does its
// damage — has its own oracles: an override that returns a stored `string`
// FIELD, whose value is re-read and PRINTED after the loop, so an over-release
// (which drives that field's refcount to zero) faults under generated-code ASan
// or prints the wrong text.
//
// NOT COVERED HERE, and why (both reproduced on this HEAD, both PRE-EXISTING
// and independent of this fix — verified by patching the fix out and observing
// the identical failure):
//   * `Nm<IArea>(rb)` where `rb` is a RECORD and `T` is explicitly bound to an
//     INTERFACE it implements — the monomorphized body SEGVs on `v.ToString()`
//     and silently returns 0 from `v.Area()`; filed separately;
//   * `new MyEx(<a +1 string producer>)` chaining `base(m)` — the message
//     string is released at the call site while the exception's Message slot
//     still points at it, a heap-use-after-free on the next `e.Message` read;
//     filed separately. Every exception in this file therefore takes a LITERAL
//     message, so that defect cannot contaminate these oracles.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

std::string out(const bezel::test::JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ---------------------------------------------------------------------------
// ARM 1 — a BUILTIN-EXCEPTION receiver (§31.1's "any class" throwable surface).
// ---------------------------------------------------------------------------

// #2867 / §8.5 + §28.9 + §31.1 — `e.ToString()` on an `Exception`-TYPED
// receiver whose dynamic type is a user subclass with a MINTING
// `override ToString()`. The static type is the built-in `Exception` (a Named
// TypeRef with no declaration), so sema resolves no MethodSymbol and codegen
// takes `lowerObjectToString`'s `isBuiltinExRecv` arm; slot 3 of the DYNAMIC
// type's vtable is `classToStringThunk`, i.e. the override, which mints.
// PRE-FIX: 711 bytes / 40 allocations over 20 iterations.
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           builtin_exception_receiver_minting_override_released_once_8_5) {
    auto r = runAotChecked(
        "module App; "
        "class MyEx: Exception "
        "  private _n: int; "
        "  public constructor(n: int) base(\"m\"); "
        "    _n := n; "
        "  end constructor; "
        "  public override method ToString(): string "
        "    return \"E\" + _n.ToString(); "
        "  end ToString; "
        "end MyEx; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var e: Exception := new MyEx(i); "
        "    var s: string := e.ToString(); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + s.Length + churn.Length; "
        "  end while; "
        "  if t <> 102 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2867 / §8.5 + §28.9 — the same receiver shape with the result in a
// CONCATENATION operand position rather than a `var` bind. The discharge site
// differs (releaseConcatIntermediate rather than the bind-transfer +
// scope-end release), and it consults the SAME recognizer — so this is a
// genuinely separate consumer of the widened predicate, not a restatement.
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           builtin_exception_receiver_concat_operand_released_once_8_5) {
    auto r = runAotChecked(
        "module App; "
        "class MyEx2: Exception "
        "  private _n: int; "
        "  public constructor(n: int) base(\"m\"); "
        "    _n := n; "
        "  end constructor; "
        "  public override method ToString(): string "
        "    return \"E\" + _n.ToString(); "
        "  end ToString; "
        "end MyEx2; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var e: Exception := new MyEx2(i); "
        "    var cat: string := \"<\" + e.ToString() + \">\"; "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + cat.Length + churn.Length; "
        "  end while; "
        "  if t <> 142 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2867 / §8.5 + §31.4 — the same arm reached through a NON-root built-in
// exception spelling. `isBuiltinExceptionTypeRef` matches the type NAME against
// the whole built-in vtable set, so `ArgumentException` takes the same arm as
// `Exception`; this pins that the widening is keyed on the set, not on the one
// name the repro happened to use.
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           builtin_exception_subtype_spelling_released_once_31_4) {
    auto r = runAotChecked(
        "module App; "
        "class MyArg: ArgumentException "
        "  private _n: int; "
        "  public constructor(n: int) base(\"m\"); "
        "    _n := n; "
        "  end constructor; "
        "  public override method ToString(): string "
        "    return \"A\" + _n.ToString(); "
        "  end ToString; "
        "end MyArg; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var e: ArgumentException := new MyArg(i); "
        "    var s: string := e.ToString(); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + s.Length + churn.Length; "
        "  end while; "
        "  if t <> 102 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// ---------------------------------------------------------------------------
// ARM 2 — a BARE GENERIC-PARAMETER receiver (§14.1: generics are monomorphized).
// ---------------------------------------------------------------------------

// #2867 / §14.1 + §8.5 + §28.9 — `v.ToString()` where `v: T` and the active
// monomorphization binds `T` to a CLASS with a minting `override ToString()`.
// The receiver's sema type is a bare, declaration-less Named TypeRef, so the
// numeric/Named blocks of the recognizer never see it; only the
// `genericSubstSemaTy_` arm can, and its Class/Interface exclusion is what this
// fix lifts. PRE-FIX: 711 bytes / 40 allocations over 20 iterations.
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           generic_param_receiver_bound_to_class_released_once_14_1) {
    auto r = runAotChecked(
        "module App; "
        "class K "
        "  private _n: int; "
        "  public constructor(n: int) "
        "    _n := n; "
        "  end constructor; "
        "  public override method ToString(): string "
        "    return \"K\" + _n.ToString(); "
        "  end ToString; "
        "end K; "
        "class C "
        "  public static method Len<T>(v: T): int where T: class "
        "    var s: string := v.ToString(); "
        "    return s.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var k := new K(i); "
        "      t := t + Len<K>(k); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + churn.Length; "
        "    end while; "
        "    if t <> 102 then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2867 / §14.1 + §8.5 — the INTERFACE half of the same arm: `T` is bound to an
// interface and the runtime value is a class implementing it, so slot 3 is the
// implementor's minting override. This is the second of the two declaration
// kinds the lifted exclusion names, and it is not implied by the class case:
// the call site's `genericSubstSemaTy_` branch tests `Class || Interface`
// separately.
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           generic_param_receiver_bound_to_interface_released_once_14_1) {
    auto r = runAotChecked(
        "module App; "
        "interface IArea "
        "  method Area(): int; "
        "end IArea; "
        "class Sq: IArea "
        "  private _n: int; "
        "  public constructor(n: int) "
        "    _n := n; "
        "  end constructor; "
        "  public method Area(): int "
        "    return _n * _n; "
        "  end Area; "
        "  public override method ToString(): string "
        "    return \"S\" + _n.ToString(); "
        "  end ToString; "
        "end Sq; "
        "class C "
        "  public static method Len<T>(v: T): int "
        "    var s: string := v.ToString(); "
        "    return s.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var q := new Sq(i); "
        "      t := t + Len<IArea>(q); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + churn.Length; "
        "    end while; "
        "    if t <> 102 then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// ---------------------------------------------------------------------------
// OVER-RELEASE oracles — the direction a "no leak" assertion cannot see.
// These PASS both with and without the fix; they are the guard that the leak
// was closed by discharging the producer's reference exactly once, not by
// discharging a reference the program still owns.
// ---------------------------------------------------------------------------

// #2867 / §28.9 + §28.3 — a builtin-exception receiver whose override returns a
// stored `string` FIELD. The ReturnStmt lowering mints `+1` on top of the
// field's own reference (§28.3: an announced return is a transfer to the
// caller), so a correct single discharge leaves the field intact;
// an over-release drives the field's refcount to zero and the post-loop read
// faults under generated-code ASan (leaks OFF here so the process exit code
// reports control flow, per the harness note on LeakSanitizer).
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           builtin_exception_field_returning_override_not_over_released_28_9) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class FieldEx: Exception "
        "  private _tag: string; "
        "  public constructor(t: string) base(\"m\"); "
        "    _tag := t; "
        "  end constructor; "
        "  public override method ToString(): string "
        "    return _tag; "
        "  end ToString; "
        "end FieldEx; "
        "class C public static method Main(): int "
        "  using var e: Exception := "
        "      new FieldEx(\"t\" + \"a\".Substring(0, 1) + \"g\"); "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var s: string := e.ToString(); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + s.Length + churn.Length; "
        "  end while; "
        "  Console.WriteLine(e.ToString()); "
        "  Console.WriteLine(t); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_TRUE(out(r).find("tag") != std::string::npos);
    BEZEL_EXPECT_TRUE(out(r).find("111") != std::string::npos);
}

// #2867 / §28.9 + §28.3 + §14.1 — the generic-parameter twin of the oracle
// above, driven through BOTH bindings the fix widens (`T` = the class, and the
// same object through an interface-typed parameter), 40 discharges in total
// before the field is re-read.
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           generic_param_field_returning_override_not_over_released_28_9) {
    auto r = runAotCheckedLenient(
        "module App; "
        "interface IShape "
        "  method Sides(): int; "
        "end IShape; "
        "class Sq2: IShape "
        "  private _tag: string; "
        "  public constructor(t: string) "
        "    _tag := t; "
        "  end constructor; "
        "  public method Sides(): int "
        "    return 4; "
        "  end Sides; "
        "  public override method ToString(): string "
        "    return _tag; "
        "  end ToString; "
        "end Sq2; "
        "class C "
        "  public static method Grind<T>(v: T): int where T: class "
        "    var i: int := 0; "
        "    var t: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      var s: string := v.ToString(); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + s.Length + churn.Length; "
        "    end while; "
        "    return t; "
        "  end Grind; "
        "  public static method GrindIface(v: IShape): int "
        "    var i: int := 0; "
        "    var t: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      var s: string := v.ToString(); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + s.Length + churn.Length; "
        "    end while; "
        "    return t; "
        "  end GrindIface; "
        "  public static method Main(): int "
        "    using var q := new Sq2(\"t\" + \"a\".Substring(0, 1) + \"g\"); "
        "    var t: int := Grind<Sq2>(q); "
        "    t := t + GrindIface(q); "
        "    Console.WriteLine(q.ToString()); "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_TRUE(out(r).find("tag") != std::string::npos);
    BEZEL_EXPECT_TRUE(out(r).find("222") != std::string::npos);
}

// ---------------------------------------------------------------------------
// SENTINEL controls — the arms whose answer is an immortal literal. These PASS
// both with and without the fix, and they are the reason the widening is safe:
// the newly-recognized predicate now returns true for them too, so a release is
// emitted where none was before. `bz_string_release` is a no-op on a constant
// global — if that were ever untrue these would abort with
// "free(): invalid pointer", exactly as #2875's console repro did.
// ---------------------------------------------------------------------------

// #2867 / §8.5 "The compiler-generated default returns: - For classes: the
// type's qualified name" — a GENUINE built-in exception instance (slot 3 is
// `bz$extostr$Exception`, a constant global), a user exception subclass with NO
// override (slot 3 is `classToStringThunk`'s class-name literal), and a `T`
// bound to a class with no override. The rendered TEXT is pinned as well as the
// lifetime: this fix must not change one character of §8.5's answer.
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           sentinel_rendering_arms_are_unchanged_and_not_over_released_8_5) {
    auto r = runAotChecked(
        "module App; "
        "class Plain: Exception "
        "  public constructor() base(\"m\"); end constructor; "
        "end Plain; "
        "class NoOv "
        "  public constructor() end constructor; "
        "end NoOv; "
        "class C "
        "  public static method Nm<T>(v: T): string "
        "    return v.ToString(); "
        "  end Nm; "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    var acc: string := \"\"; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var be: Exception := new Exception(\"m\"); "
        "      using var pe: Exception := new Plain(); "
        "      using var nv := new NoOv(); "
        "      acc := be.ToString() + \"|\" + pe.ToString() + \"|\" "
        "             + Nm<NoOv>(nv); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      if churn.Length < 1 then return 9; end if; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_TRUE(out(r).find("Exception|Plain|NoOv") != std::string::npos);
}

// #2867 control — the substitutions #2475 ALREADY recognized (a value
// primitive, a positional record, and the deliberately-excluded enum) must
// still behave exactly as before: this fix adds a branch after theirs and must
// not shadow or disturb them. Loop-amplified and leak-checked like the rest.
BEZEL_TEST(codegen_object_tostring_producer_residual_2867,
           generic_param_primitive_record_enum_substitutions_unchanged_2475) {
    auto r = runAotChecked(
        "module App; "
        "enum Color Red, Green; end Color; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Nm<T>(v: T): int "
        "    var s: string := v.ToString(); "
        "    return s.Length; "
        "  end Nm; "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      t := t + Nm<int>(i) + Nm<Pt>(new Pt(i, i)) "
        "             + Nm<Color>(Color.Green); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + churn.Length; "
        "    end while; "
        "    if t < 1 then return 1; end if; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}
