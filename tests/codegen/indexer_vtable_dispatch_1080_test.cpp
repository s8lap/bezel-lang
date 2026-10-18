// Regression tests for #1080 (§23.4 / §12.1): a virtual/override/abstract
// user-defined INDEXER getter (and setter) dispatched STATICALLY for class
// receivers and `this[i]`, so a `base[i]` read through a base-typed reference,
// and a `this[i]` read inside a base-class method, ran the wrong (base /
// abstract) implementation. A direct `obj[i]` on the concrete type already
// dispatched correctly.
//
// Root cause: indexers were excluded from vtable slots (isVirtualProperty
// returns false for indexers), so there was no slot to dispatch through and
// lowerIndexerGet resolved the getter body from the compile-time receiver type.
// The fix mirrors the #481 property-vtable path: virtual indexers get vtable
// slots (keyed by index signature so overloads stay distinct), filled with the
// concrete class's indexer getter/setter, and lowerIndexerGet/Set dispatch
// through the slot for class receivers.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1080 form A: `base[i]` through a base-typed reference dispatches to the
// most-derived override.
BEZEL_TEST(indexer_vtable_dispatch_1080, base_ref_get_dispatches_override) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual property [i: int]: int => 1; end Base; "
        "class Derived: Base "
        "  public override property [i: int]: int => 99; end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var b: Base := d; "
        "  return b[0]; "                                // 99 (was 1)
        "end Main; end C;"), 99);
}

// #1080 form B: `this[i]` inside an (abstract) base-class method dispatches to
// the concrete override.
BEZEL_TEST(indexer_vtable_dispatch_1080, this_index_in_base_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class ABase "
        "  public abstract property [i: int]: int; "
        "  public method GetZero(): int => this[0]; "
        "end ABase; "
        "class Concrete: ABase "
        "  public override property [i: int]: int => 42; end Concrete; "
        "class C public static method Main(): int "
        "  using var c := new Concrete(); "
        "  return c.GetZero(); "                         // 42 (was 0)
        "end Main; end C;"), 42);
}

// The SETTER also dispatches dynamically: a `base[i] := v` through a base-ref
// runs the override's setter, and the subsequent read runs the override getter.
BEZEL_TEST(indexer_vtable_dispatch_1080, base_ref_setter_dispatches_override) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public _v: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _v + 1; "
        "    set => do _v := value; end; "
        "  end; "
        "end Base; "
        "class Derived: Base "
        "  public override property [i: int]: int "
        "    get => _v + 100; "
        "    set => do _v := value * 2; end; "
        "  end; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var b: Base := d; "
        "  b[0] := 5; "                                  // override setter → _v = 10
        "  return b[0]; "                                // override getter → 110
        "end Main; end C;"), 110);
}

// Regression guard: a direct read on the concrete type is unchanged.
BEZEL_TEST(indexer_vtable_dispatch_1080, direct_override_read_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual property [i: int]: int => 1; end Base; "
        "class Derived: Base "
        "  public override property [i: int]: int => 42; end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return d[0]; "                                // 42
        "end Main; end C;"), 42);
}

// Regression guard: a plain (non-virtual) indexer is unchanged (static path).
BEZEL_TEST(indexer_vtable_dispatch_1080, non_virtual_indexer_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 7; "
        "  public property [i: int]: int => _v + i; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  return b[3]; "                                // 10
        "end Main; end C;"), 10);
}

// Overloaded virtual indexers (`[i: int]` / `[s: bool]`) get DISTINCT vtable
// slots (keyed by index signature) so each dispatches to its own getter.
BEZEL_TEST(indexer_vtable_dispatch_1080, overloaded_virtual_indexers_distinct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual property [i: int]: int => 1; "
        "  public virtual property [s: bool]: int => 2; "
        "end Base; "
        "class C public static method Main(): int "
        "  using var b := new Base(); "
        "  return b[0] + b[true]; "                      // 1 + 2 = 3
        "end Main; end C;"), 3);
}


// ---------------------------------------------------------------------------
// #6319 (§23.4 / §14.1, v0.338) — the dispatched accessor's signature must be
// built from the RECEIVER'S SUBSTITUTED declaration.
//
// The three dispatch sites below are the ones #1080 introduced and the ones
// #6319 fixed: lowerIndexerGet's class-vtable branch, lowerIndexerSet's, and
// lowerIndexerCompoundCore's. Each built `retTy` and `paramTys` by lowering
// the indexer's DECLARED `pd->type` / `pd->indexerParams[k].type` directly.
// §14.1 binds a generic class's declared type parameters to the ARGUMENTS at
// the use site, so on a `VB<Array<int>>` receiver the declared `[i: T]` must
// read as `Array<int>`. Reading it as the declared `T` yields the non-null i32
// generic-erasure placeholder, while the vtable slot holds the OVERRIDE's
// getter (`[i: Array<U>]` → ptr) — a pointer passed against an i32 prototype,
// which LLVM's verifier rejects outright ("Call parameter type does not match
// function signature"). The generic ELEMENT type is worse: both sides are
// i32-shaped only by accident of the placeholder, so nothing aborts and the
// call returns garbage.
//
// Each row's program FAILED to compile (or returned the wrong value) before
// the fix. Same suite file because it is the same branch of the same feature
// the #1080 rows above pin.
// ---------------------------------------------------------------------------

// The issue's own repro, in full: a base-typed GET of a virtual indexer whose
// key is a type parameter bound to a NESTED type (`VB<T>`'s `[i: T]` through
// `E<U,V>: VB<Array<U>>`), reached through a `VB<Array<int>>` receiver. The
// derived-receiver read is unchanged by the fix (the override's own declared
// parameter already matched), which is exactly the asymmetry the issue reports.
BEZEL_TEST(indexer_vtable_dispatch_6319, base_typed_get_substitutes_generic_key) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class VB<T> "
        "  public constructor() end constructor; "
        "  public virtual property [i: T]: string => \"bidx\"; "
        "end VB; "
        "public class E<U, V>: VB<Array<U>> "
        "  public constructor() end constructor; "
        "  public override property [i: Array<U>]: string => \"didx\"; "
        "end E; "
        "public class Program public static method Main(): int "
        "  using var e := new E<int, string>(); "
        "  const a: Array<int> := [1, 2]; "
        "  var b: VB<Array<int>> := e; "
        "  if b[a] = \"didx\" then return 1; end; "
        "  return 0; "                                 // 1 (pre-fix: verify failure)
        "end Main; end Program;"), 1);
}

// The SETTER twin: same defect on the `void set(ptr this, idx..., value)`
// signature lowerIndexerSet builds. Covers both the setter's dispatch and the
// getter's read-back that observes it.
BEZEL_TEST(indexer_vtable_dispatch_6319, base_typed_set_substitutes_generic_key) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class VB<T> "
        "  private _v: string := \"b\"; "
        "  public constructor() end constructor; "
        "  public virtual property [i: T]: string "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end VB; "
        "public class E<U, V>: VB<Array<U>> "
        "  private _w: string := \"d\"; "
        "  public constructor() end constructor; "
        "  public override property [i: Array<U>]: string "
        "    get => _w; "
        "    set => _w := value; "
        "  end; "
        "end E; "
        "public class Program public static method Main(): int "
        "  using var e := new E<int, string>(); "
        "  const a: Array<int> := [1, 2]; "
        "  var b: VB<Array<int>> := e; "
        "  b[a] := \"via-base\"; "
        "  if b[a] = \"via-base\" then return 1; end; "
        "  return 0; "                                 // 1 (pre-fix: verify failure)
        "end Main; end Program;"), 1);
}

// The COMPOUND-ASSIGN twin: `b[a] += v` needs BOTH signatures under the
// substitution — the getter it reads through and the setter it writes back
// through — so this row fails if either half regresses.
BEZEL_TEST(indexer_vtable_dispatch_6319, base_typed_compound_substitutes_generic_key) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class VB<T> "
        "  private _v: string := \"b\"; "
        "  public constructor() end constructor; "
        "  public virtual property [i: T]: string "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end VB; "
        "public class E<U, V>: VB<Array<U>> "
        "  private _w: string := \"d\"; "
        "  public constructor() end constructor; "
        "  public override property [i: Array<U>]: string "
        "    get => _w; "
        "    set => _w := value; "
        "  end; "
        "end E; "
        "public class Program public static method Main(): int "
        "  using var e := new E<int, string>(); "
        "  const a: Array<int> := [1, 2]; "
        "  var b: VB<Array<int>> := e; "
        "  b[a] += \"!\"; "
        "  if b[a] = \"d!\" then return 1; end; "
        "  return 0; "                                 // 1 (pre-fix: verify failure)
        "end Main; end Program;"), 1);
}

// The generic ELEMENT type — the SILENT row. `VB<T>`'s declared `[i: int]: T`
// with `VB<string>` must be typed as the string reference (ptr), not as the
// erased `T` placeholder (i32). Pre-fix nothing aborted: the call returned an
// i32 read of a pointer slot and this program printed `0`.
BEZEL_TEST(indexer_vtable_dispatch_6319, base_typed_get_substitutes_generic_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class VB<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public virtual property [i: int]: T => _v; "
        "end VB; "
        "public class E<V>: VB<V> "
        "  public constructor() base(\"didx\"); end constructor; "
        "  public override property [i: int]: V => _v; "
        "end E; "
        "public class Program public static method Main(): int "
        "  using var e := new E<string>(); "
        "  var b: VB<string> := e; "
        "  if b[0] = \"didx\" then return 1; end; "
        "  return 0; "                                 // 1 (pre-fix: returned 0)
        "end Main; end Program;"), 1);
}

// The ANCESTOR half: `Root<T>` reached through the INTERMEDIATE `Mid<int>`
// (`Mid<U>: Root<Array<U>>`), so the substitution the signature needs is
// `T := Array<U>` composed with `U := int` — a two-link chain, not the
// receiver's own argument. Read through BOTH `Mid<int>` and `Root<Array<int>>`.
BEZEL_TEST(indexer_vtable_dispatch_6319, ancestor_chain_substitutes_generic_key) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Root<T> "
        "  public constructor() end constructor; "
        "  public virtual property [i: T]: string => \"root\"; "
        "end Root; "
        "public class Mid<U>: Root<Array<U>> "
        "  public constructor() end constructor; "
        "end Mid; "
        "public class Leaf<V>: Mid<V> "
        "  public constructor() end constructor; "
        "  public override property [i: Array<V>]: string => \"leaf\"; "
        "end Leaf; "
        "public class Program public static method Main(): int "
        "  using var l := new Leaf<int>(); "
        "  const a: Array<int> := [1, 2]; "
        "  var m: Mid<int> := l; "
        "  if m[a] = \"leaf\" then "
        "    var r: Root<Array<int>> := l; "
        "    if r[a] = \"leaf\" then return 1; end; "
        "  end; "
        "  return 0; "                                 // 1 (pre-fix: verify failure)
        "end Main; end Program;"), 1);
}

// TWO index parameters: the substituted key sits at `paramTys[1]` and the
// concrete `int` at `paramTys[2]`, so this row pins that the substitution
// lands on the right slot of the parameter list (and that the list is not
// silently truncated to the substituted slot's arity).
BEZEL_TEST(indexer_vtable_dispatch_6319, base_typed_two_key_setter_substitutes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class VB<T> "
        "  private _v: string := \"b\"; "
        "  public constructor() end constructor; "
        "  public virtual property [i: T, j: int]: string "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end VB; "
        "public class E<U, V>: VB<Array<U>> "
        "  private _w: string := \"d\"; "
        "  public constructor() end constructor; "
        "  public override property [i: Array<U>, j: int]: string "
        "    get => _w; "
        "    set => _w := value; "
        "  end; "
        "end E; "
        "public class Program public static method Main(): int "
        "  using var e := new E<int, string>(); "
        "  const a: Array<int> := [1, 2]; "
        "  var b: VB<Array<int>> := e; "
        "  b[a, 7] := \"two\"; "
        "  if b[a, 7] = \"two\" then return 1; end; "
        "  return 0; "                                 // 1 (pre-fix: verify failure)
        "end Main; end Program;"), 1);
}

// DIRECT instantiation of the generic base: `new VB<Array<int>>()` with no
// derived class at all. Same call site as row 1 (there is no override to
// dispatch to), so it pins that the substitution comes from the RECEIVER
// rather than from anything about the class hierarchy.
BEZEL_TEST(indexer_vtable_dispatch_6319, direct_instantiation_substitutes_generic_key) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class VB<T> "
        "  public constructor() end constructor; "
        "  public virtual property [i: T]: string => \"bidx\"; "
        "end VB; "
        "public class Program public static method Main(): int "
        "  using var v := new VB<Array<int>>(); "
        "  const a: Array<int> := [1, 2]; "
        "  if v[a] = \"bidx\" then return 1; end; "
        "  return 0; "                                 // 1 (pre-fix: verify failure)
        "end Main; end Program;"), 1);
}

// ---------------------------------------------------------------------------
// #6304 (§23.4 / §14.1, v0.338) — the ELEMENT-TYPE face of #6319, filed
// separately and fixed by the same change. #6319's rows pin a generic KEY
// whose binding is a pointer; these pin a generic ELEMENT, which is the shape
// that fails SILENTLY: both the declared `T` and the substituted type can be
// i32-shaped by accident of the erasure placeholder, so nothing aborts and the
// call returns a pointer read as an integer. The issue's author measured
// "2349200" where "k" was expected.
// ---------------------------------------------------------------------------

// Repro 1 from #6304, verbatim in shape: `B<T>`'s declared element `T`
// substituted to `string` through a `B<string>` receiver.
BEZEL_TEST(indexer_vtable_dispatch_6304, base_typed_get_substitutes_string_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class B<T> "
        "  public constructor() end constructor; "
        "  public virtual property [i: int]: T => default(T); "
        "end B; "
        "public class E: B<string> "
        "  public constructor() end constructor; "
        "  public override property [i: int]: string => \"k\"; "
        "end E; "
        "public class Program public static method Main(): int "
        "  var b: B<string> := new E(); "
        "  if b[1] = \"k\" then return 1; end; "
        "  return 0; "                                 // 1 (pre-fix: a pointer-sized int)
        "end Main; end Program;"), 1);
}

// Repro 2 from #6304 — the element declared as `T` and substituted to a WRAPPED
// type (`B<Array<int>>`) — is DELIBERATELY NOT PINNED HERE. It reaches a
// SECOND, independent defect on the PRODUCER side: `ensureIndexerGetterFn`
// lowers the override's BODY against the erased `classStruct(B)` layout, so the
// inherited `T`-typed field is read as the i32 placeholder while the accessor
// is declared `ptr`:
//     define internal ptr @"bz$indexer$get.2"(ptr %0, i32 %1) {
//       %3 = getelementptr inbounds nuw %class.B, ptr %0, i32 0, i32 2
//       %4 = load i32, ptr %3, align 8        ; the slot is ptr at T := Array<int>
//       ret i32 %4                           ; "Function return type does not
//                                            ;  match operand type of return inst"
// That is the #3353 family (`emitImportedIndexerGetterInheritedWrapper` exists
// for the cross-module face of exactly this hazard) and is NOT fixed by the
// #6319 call-site change: it reproduces through a DERIVED receiver alone,
// which never reaches any of the three dispatch sites this file's rows cover.
// Filed separately as its own issue; a row here would be RED for a reason this
// commit does not address, which is worse than no row.

// The third form, reported on the issue by the #6308 lane: a virtual indexer
// with a generic KEY *and* a generic ELEMENT, on a generic class, with NO
// override and NO abstract — so the dispatched accessor is the base's own
// lowered body and there is no hierarchy in the picture at all. That isolates
// the substitution to the receiver alone.
BEZEL_TEST(indexer_vtable_dispatch_6304, virtual_generic_key_and_element_no_override) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class B<X, Y> "
        "  private _v: Y; "
        "  public constructor(v: Y) _v := v; end constructor; "
        "  public virtual property [i: X]: Y => _v; "
        "end B; "
        "public class Program public static method Main(): int "
        "  using var b := new B<string, string>(\"hello\"); "
        "  if b[\"x\"] = \"hello\" then return 1; end; "
        "  return 0; "                                 // 1 (pre-fix: a pointer-sized int)
        "end Main; end Program;"), 1);
}
