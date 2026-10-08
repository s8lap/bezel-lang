// Codegen — a nullable BARE TYPE-PARAMETER return (`T?`) FORWARDED through an
// outer generic's own `U?` return (issue #2743, spec v0.240).
//
// §7.10 (Generics and nullability): "`T?` always means \"the nullable form of
// T,\" regardless of constraint." §14.1 (Generic declarations): a generic body is
// monomorphized per instantiation; the unspecialized template is dead code that
// must nevertheless emit valid IR.
//
// Root cause (before fix), in Emitter::lowerTypeRef (LlvmBackend_Layout.cpp, the
// #568 nullable-type-parameter branch): when an inner generic's specialization is
// built LAZILY from inside an OUTER generic's unspecialized TEMPLATE body, the
// active substitution maps one type parameter onto ANOTHER, still-unbound one
// (`Inner<T>` reached as `Inner<U>` from `OuterU<U>`'s template → {T -> U}).
// `nullableTypeParamConcrete` reports SUCCESS for that mapping — it merely looks
// the name up in `genericSubstSemaTy_` — but the result is not concrete. The
// branch then tested only `isNullableValueTy(resolved)`, and on the false answer
// fell through under the reading "resolved to a reference type → plain ptr `?`
// lowering". `U` is neither: it is still a type parameter. The fall-through
// reached lowerTypeRef's Named case, which resolved `T` to U's PLACEHOLDER LLVM
// type (i32) and DROPPED the nullability. The pseudo-specialization
// `Inner$Head$spec` was therefore typed to return `i32` while its body's
// `return null;` emitted `ret ptr null` — "Function return type does not match
// operand type of return inst!", `bezelc -c` exit 1 on a fully LEGAL program.
// (Its caller compounded this: receiving a bare `i32` for a `U?` it wrapped the
// result as tag=1, i.e. unconditionally present, discarding the null case.)
//
// The four shapes that do NOT fire pin the diagnosis: a single generic with no
// wrapper builds no such pseudo-specialization; a non-nullable chain never enters
// the branch; a non-generic chain has no unbound substitution; and an inner body
// whose value comes from a `T?` FIELD (rather than `return null;`) mis-lowers the
// field read the SAME way, so signature and body agree and no mismatch surfaces.
// The collection indexer in the issue's repro is incidental — see the minimal
// case below, which has no collection at all.
//
// After fix: a substitution that resolves a type parameter onto another unbound
// type parameter takes the TEMPLATE rule — the placeholder nullable value struct
// {i8, i32} — exactly as the no-binding branch below it already did. Signature,
// `return <value>` wrap and `return null;` then all agree, and the forwarding
// caller passes the tagged value through instead of re-tagging it.
//
// Every test here FAILS to compile before the fix (IR verification) and produces
// the spec-stated value after.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bezel::test::JitResult jit(const std::string& prog) { return runAot(prog); }

// For the two heap-pointer-builtin cases below: a mis-lowered `Stopwatch?`
// carries a `bz_stopwatch_t*` in a 32-bit payload, so force generated-code
// ASan — a truncated pointer that survives IR verification must not pass
// silently. -999 (a value no test below returns) marks a crash/leak.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── the issue's own repro: forwarded `T?` sourced from an owning-collection
//    indexer. Main returns 7 (the head is present). ──────────────────────────
BEZEL_TEST(codegen_generic_nullable_forward_2743, forward_owning_indexer_7_10) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "class Res public n: int := 0; end Res; "
        "class Inner<T> where T: class "
        "  private owned _a: OwningArray<T> := new OwningArray<T>(); "
        "  public method Put(owned x: T): int "
        "    _a.Add(move x); "
        "    return _a.Count; "
        "  end Put; "
        "  public method Head(): T? "
        "    if _a.Count > 0 then "
        "      return _a[0]; "
        "    end; "
        "    return null; "
        "  end Head; "
        "end Inner; "
        "class OuterU<U> where U: class "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method Put(owned x: U): int "
        "    return g.Put(move x); "
        "  end Put; "
        "  public method Head(): U? "
        "    return g.Head(); "
        "  end Head; "
        "end OuterU; "
        "class C public static method Main(): int "
        "  using var o := new OuterU<Res>(); "
        "  o.Put(new Res()); "
        "  var s := o.Head(); "
        "  if s <> null then return 7; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── MINIMAL trigger: no collection, no indexer, no `Put`. The conjunction the
//    issue reported was over-specified — an outer generic forwarding to an inner
//    generic's `T?` whose body contains `return null;` is the whole of it. ────
BEZEL_TEST(codegen_generic_nullable_forward_2743, minimal_forward_return_null_7_10) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "class Res public n: int := 0; end Res; "
        "class Inner<T> where T: class "
        "  public method Head(): T? "
        "    return null; "
        "  end Head; "
        "end Inner; "
        "class OuterU<U> where U: class "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method Head(): U? "
        "    return g.Head(); "
        "  end Head; "
        "end OuterU; "
        "class C public static method Main(): int "
        "  using var o := new OuterU<Res>(); "
        "  var s := o.Head(); "
        "  if s = null then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── THREE levels of forwarding: the substitution chains parameter-to-parameter
//    twice (L1<U> → L2<V> → L3<T>). ────────────────────────────────────────────
BEZEL_TEST(codegen_generic_nullable_forward_2743, three_level_forward_7_10) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "class Res public n: int := 0; end Res; "
        "class L3<T> where T: class "
        "  public method Head(): T? return null; end Head; "
        "end L3; "
        "class L2<V> where V: class "
        "  private owned g: L3<V> := new L3<V>(); "
        "  public method Head(): V? return g.Head(); end Head; "
        "end L2; "
        "class L1<U> where U: class "
        "  private owned g: L2<U> := new L2<U>(); "
        "  public method Head(): U? return g.Head(); end Head; "
        "end L1; "
        "class C public static method Main(): int "
        "  using var o := new L1<Res>(); "
        "  var s := o.Head(); "
        "  if s = null then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── the same forward instantiated at a VALUE type. §7.10: `T?` is the nullable
//    form of T "regardless of constraint" — so an unconstrained parameter bound
//    to `int` must forward exactly as a class-constrained one does. ───────────
BEZEL_TEST(codegen_generic_nullable_forward_2743, forward_value_type_arg_7_10) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "class Inner<T> "
        "  public method Head(): T? return null; end Head; "
        "end Inner; "
        "class OuterU<U> "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method Head(): U? return g.Head(); end Head; "
        "end OuterU; "
        "class C public static method Main(): int "
        "  using var o := new OuterU<int>(); "
        "  var s := o.Head(); "
        "  if s = null then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── the WIDTH discriminator, and the reason every other case here is not one.
//    The placeholder this fix installs is `{i8, i32}`. At a CLASS type argument
//    the live specialization is `ptr`, and at `int` it is `{i8, i32}` — i.e.
//    LITERALLY THE PLACEHOLDER, since LLVM uniques anonymous structs
//    structurally. So no test above can tell a live specialization from the dead
//    pseudo-specialization being called: at `int` they are the same type. This
//    is the same int-vacuity trap recorded for #2755, applied to this fix's own
//    coverage. At `long` the live specialization is `{i8, i64}`, so the two are
//    distinguishable and the claim that the template is never called becomes
//    observable rather than assumed.
//
//    Two forwards in one program: `Empty()` forwards `return null;` (the
//    original defect) and `Head()` forwards a PRESENT 64-bit value. The value
//    is 2^32 + 7, which truncates to exactly 7 in an i32 payload — so a
//    placeholder-width payload that somehow survived IR verification would
//    still be caught here, by the value rather than by the verifier.
BEZEL_TEST(codegen_generic_nullable_forward_2743, forward_long_type_arg_width_7_10) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "class Inner<T> "
        "  private _v: T? := null; "
        "  public method Put(v: T) _v := v; end Put; "
        "  public method Head(): T? return _v; end Head; "
        "  public method Empty(): T? return null; end Empty; "
        "end Inner; "
        "class OuterU<U> "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method Put(v: U) g.Put(v); end Put; "
        "  public method Head(): U? return g.Head(); end Head; "
        "  public method Empty(): U? return g.Empty(); end Empty; "
        "end OuterU; "
        "class C public static method Main(): int "
        "  using var o := new OuterU<long>(); "
        "  if o.Empty() <> null then return 1; end; "
        "  o.Put(4294967303L); "
        "  var s := o.Head(); "
        "  if s = null then return 2; end; "
        "  if s! <> 4294967303L then return 3; end; "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── an INTERFACE-bound parameter. §7.10: "Inside the generic body, T is
//    non-nullable under any type-bound constraint" — `T?` is still its nullable
//    form, and the forward must lower identically. ─────────────────────────────
BEZEL_TEST(codegen_generic_nullable_forward_2743, forward_interface_constraint_7_10) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "interface IFoo method Tag(): int; end IFoo; "
        "class Res: IFoo public method Tag(): int return 5; end Tag; end Res; "
        "class Inner<T> where T: IFoo "
        "  public method Head(): T? return null; end Head; "
        "end Inner; "
        "class OuterU<U> where U: IFoo "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method Head(): U? return g.Head(); end Head; "
        "end OuterU; "
        "class C public static method Main(): int "
        "  using var o := new OuterU<Res>(); "
        "  var s := o.Head(); "
        "  if s = null then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── NON-REGRESSION: the negative controls must stay accepted, and in particular
//    the `T?`-FIELD source (which never fired) must keep forwarding the right
//    VALUE — the pre-fix caller re-tagged a forwarded result as unconditionally
//    present, so a test that only asserted "compiles" would not have seen it.
//    Here the field is null, so the forwarded result must compare EQUAL to null.
BEZEL_TEST(codegen_generic_nullable_forward_2743, forward_nullable_field_value_7_10) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "class Res public n: int := 0; end Res; "
        "class Inner<T> where T: class "
        "  private _v: T? := null; "
        "  public method Head(): T? return _v; end Head; "
        "end Inner; "
        "class OuterU<U> where U: class "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method Head(): U? return g.Head(); end Head; "
        "end OuterU; "
        "class C public static method Main(): int "
        "  using var o := new OuterU<Res>(); "
        "  var s := o.Head(); "
        "  if s = null then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── NON-REGRESSION: a single generic with no wrapper (the shape that always
//    worked) still produces the right value — the fix must not disturb the
//    concrete `$spec$<Type>` path, only the unbound-parameter one. ────────────
BEZEL_TEST(codegen_generic_nullable_forward_2743, single_generic_no_wrapper_14_1) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "class Res public n: int := 0; end Res; "
        "class Inner<T> where T: class "
        "  private owned _a: OwningArray<T> := new OwningArray<T>(); "
        "  public method Put(owned x: T): int "
        "    _a.Add(move x); return _a.Count; "
        "  end Put; "
        "  public method Head(): T? "
        "    if _a.Count > 0 then return _a[0]; end; "
        "    return null; "
        "  end Head; "
        "end Inner; "
        "class C public static method Main(): int "
        "  using var o := new Inner<Res>(); "
        "  o.Put(new Res()); "
        "  var s := o.Head(); "
        "  if s <> null then return 7; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── GUARD on the fix's own predicate. `isBareTypeParamRef` decides "bare type
//    parameter" by a NEGATIVE name test against `isBuiltinTypeRefName`, so a
//    builtin MISSING from that list is misread as a type parameter. The list
//    had drifted from the registry by 17 names; `Stopwatch` (stdlib §8.1, a
//    heap-allocated `bz_stopwatch_t*`) was one of them, so a concrete
//    `Box<Stopwatch>`'s `T?` took the {i8, i32} placeholder instead of `ptr`.
//    §7.10, v0.240: `T?` is "the nullable form of T" — at T = Stopwatch that is
//    a nullable REFERENCE, i.e. a plain pointer, never a tagged value struct.
//    This case compiled and returned 7 BEFORE #2743 and must keep doing so; the
//    first draft of the fix regressed it to an IR-verify failure.
BEZEL_TEST(codegen_generic_nullable_forward_2743, type_param_bound_to_heap_builtin_7_10) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class Box<T> where T: class "
        "  private _v: T? := null; "
        "  public method Set(v: T) _v := v; end Set; "
        "  public method Get(): T? return _v; end Get; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var s := new Stopwatch(); "
        "  using var b := new Box<Stopwatch>(); "
        "  b.Set(s); "
        "  var g := b.Get(); "
        "  if g = null then return 1; end; "
        "  s.Start(); "
        "  if g!.IsRunning then return 7; end; "
        "  return 2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(e, 7);
}

// ── the SAME list gap, reached WITHOUT any generic — a directly-written
//    `Stopwatch?`. Pre-existing and independent of the forwarding defect (it
//    failed on 0b9693e too); closed by the same registry unification. Before:
//    "Call parameter type does not match function signature! %nv.val =
//    extractvalue { i8, i32 } ... call i32 @bz_stopwatch_is_running(i32 ...)".
//    §7.1 (Non-nullable and nullable types) / stdlib §8.1. Filed as #2757.
BEZEL_TEST(codegen_generic_nullable_forward_2743, direct_nullable_heap_builtin_7_1) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var s := new Stopwatch(); "
        "  var o: Stopwatch? := s; "
        "  if o = null then return 1; end; "
        "  s.Start(); "
        "  if o!.IsRunning then return 7; end; "
        "  return 2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(e, 7);
}
