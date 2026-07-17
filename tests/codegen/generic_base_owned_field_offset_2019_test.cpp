// Codegen — issue #2019 (§14 monomorphization / §11.7 inheritance-disposal /
// §28.4 base-chain disposal, v0.197): a generic BASE class holding an `owned`
// field whose OFFSET depends on a type parameter SEGV'd when a concrete derived
// instance was disposed.
//
// Shape: `class D<T> : B<T>` where `B<T>` lays out a WIDTH-VARYING value field
// (`bval: T`) BEFORE an owned CONCRETE field (`owned bf: Cell`). The base's owned
// field offset therefore shifts with T's width. Disposing a plain `D<long>` chained
// to the base's disposer, and the base disposer selected was the SHARED, type-
// erased template `bz$B$DisposeCore` — emitted once against the erased layout
// (T -> the i32 placeholder). For T=long the real base portion is laid out T -> i64,
// so `bf` sits at a different offset than the template computed: the template
// disposer freed/read the WRONG offset -> SEGV in bz$B$DisposeCore (confirmed on
// base 19b6566, ASan: "SEGV on unknown address ... in bz$B$DisposeCore").
//
// This is DISTINCT from #816 (which handled an owned TYPE-PARAMETER field, `owned
// _f: T`, that the erased disposer SKIPS -> leak). Here the owned field is a
// concrete `Cell`; the erased disposer does NOT skip it, it disposes it at a wrong
// offset. The pre-#2019 specialization gate in emitBaseDisposeChain only fired for
// `classHasOwnedTypeParamField(base)`, so this width-varying-offset case fell
// through to the template disposer.
//
// THE FIX (LlvmBackend_Disposal.cpp emitBaseDisposeChain): for a generic base,
// compare the base's instance-field storage-type sequence under the erased template
// layout (empty subst) against the concrete instantiation's layout. When they
// differ — OR the #816 owned-type-param condition holds — emit/call a SPECIALIZED
// base core disposer under the concrete substitution, so every owned base field is
// disposed exactly once at its CORRECT offset.
//
// Oracle: the destructor-counter `Cell` (ctor ++, dtor --). Each `Main` runs the
// `using var` inside a `do ... end` nested scope, so disposal completes BEFORE
// `return Cell.n`. Exit 0 == every Cell disposed exactly once; a leak returns a
// positive count; a double-free / wrong-offset free is caught by runAotChecked's
// GENERATED-CODE AddressSanitizer (or drives the counter negative). Fail-without
// (base 19b6566): the width-varying cases SEGV in bz$B$DisposeCore (r.ran == false).
// Pass-with: every case runs clean and returns 0.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + generated-code ASan oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;  // a SEGV/crash makes r.ran false -> -999
}

// Destructor-counter Cell: ctor ++, dtor --. live==0 at the end == exact-once.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// B<T>: a width-varying value field `bval: T` laid out BEFORE the owned concrete
// `bf: Cell`. D<T> : B<T> adds a derived field but no owned base override.
const char* kBaseDerived =
    "class B<T> public bval: T; public owned bf: Cell? := null; "
    "  public constructor(v: T) this.bval := v; this.bf := new Cell(); end constructor; "
    "  destructor end destructor; end B; "
    "class D<T> : B<T> public dval: int := 3; "
    "  public constructor(v: T) base(v); end constructor; "
    "  destructor end destructor; end D; ";
}  // namespace

// ===========================================================================
// #2019 — the concrete-repro: the base disposer must hit the owned field at the
// SPECIALIZED (T=long, i64) offset, not the erased template (i32) offset.
// ===========================================================================

// T = long (i64): the value field is 8 bytes, shifting `bf` past the template's
// i32-based offset. FAIL-WITHOUT: SEGV in bz$B$DisposeCore. PASS-WITH: disposed
// once, exit 0.
BEZEL_TEST(generic_base_owned_field_offset_2019,
           base_owned_field_after_width_varying_long_disposes_once_spec_28_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kBaseDerived +
        "class C public static method Main(): int "
        "  do using var d := new D<long>(1234567890123L); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// T = string (ptr, 8 bytes): a different width-varying layout that likewise shifts
// the owned base field. Disposed exactly once at the specialized offset.
BEZEL_TEST(generic_base_owned_field_offset_2019,
           base_owned_field_after_width_varying_string_disposes_once_spec_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kBaseDerived +
        "class C public static method Main(): int "
        "  do using var d := new D<string>(\"a-longish-value-string\"); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// T = a class (ptr): the type arg is itself a reference type. Disposed once.
BEZEL_TEST(generic_base_owned_field_offset_2019,
           base_owned_field_after_class_typed_param_disposes_once_spec_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Payload public v: int := 7; end Payload; "
        "class B<T> public bval: T?; public owned bf: Cell? := null; "
        "  public constructor() this.bf := new Cell(); end constructor; "
        "  destructor end destructor; end B; "
        "class D<T> : B<T> public dval: int := 3; "
        "  public constructor() base(); end constructor; "
        "  destructor end destructor; end D; "
        "class C public static method Main(): int "
        "  do using var d := new D<Payload>(); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// ===========================================================================
// Coverage of the disposal geometry the fix must keep correct.
// ===========================================================================

// Owned fields in BOTH the generic base and the generic derived class — each
// disposed exactly once (base via the specialized base core, derived via its own
// core). The base's `bf` and the derived's `df` sit at width-dependent offsets.
BEZEL_TEST(generic_base_owned_field_offset_2019,
           owned_fields_in_base_and_derived_each_disposed_once_spec_11_7) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class B<T> public bval: T; public owned bf: Cell? := null; "
        "  public constructor(v: T) this.bval := v; this.bf := new Cell(); end constructor; "
        "  destructor end destructor; end B; "
        "class D<T> : B<T> public dval: T; public owned df: Cell? := null; "
        "  public constructor(v: T) base(v); this.dval := v; this.df := new Cell(); end constructor; "
        "  destructor end destructor; end D; "
        "class C public static method Main(): int "
        "  do using var d := new D<long>(999999999999L); end; "
        "  return Cell.n; "  // 0 (two Cells, each disposed once)
        "end Main; end C;"), 0);
}

// MULTIPLE owned base fields, each preceded by a width-varying value field, so both
// offsets depend on T. Both must be freed exactly once at the specialized offsets.
BEZEL_TEST(generic_base_owned_field_offset_2019,
           multiple_owned_base_fields_each_disposed_once_spec_28_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class B<T> public b1: T; public owned f1: Cell? := null; "
        "  public b2: T; public owned f2: Cell? := null; "
        "  public constructor(v: T) this.b1 := v; this.f1 := new Cell(); "
        "    this.b2 := v; this.f2 := new Cell(); end constructor; "
        "  destructor end destructor; end B; "
        "class D<T> : B<T> public dval: int := 3; "
        "  public constructor(v: T) base(v); end constructor; "
        "  destructor end destructor; end D; "
        "class C public static method Main(): int "
        "  do using var d := new D<long>(123456789012L); end; "
        "  return Cell.n; "  // 0 (two base Cells, each disposed once)
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// Not-a-regression controls.
// ---------------------------------------------------------------------------

// T = int (the erased template's default width): the specialized layout MATCHES the
// template, so the fix's layout comparison leaves the template-disposer path in
// place (or an identical specialized core) — this is the case that happened to work
// before #2019. Must still dispose exactly once.
BEZEL_TEST(generic_base_owned_field_offset_2019,
           default_width_int_param_still_disposes_once_spec_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kBaseDerived +
        "class C public static method Main(): int "
        "  do using var d := new D<int>(42); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// A NON-generic base `D : B` (no type parameters anywhere): the layout is fixed, so
// the fix leaves the ordinary template base disposer in place. Unaffected — disposed
// exactly once.
BEZEL_TEST(generic_base_owned_field_offset_2019,
           nongeneric_base_owned_field_unaffected_disposes_once_spec_11_7) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class B public bval: long := 0L; public owned bf: Cell? := null; "
        "  public constructor() this.bf := new Cell(); end constructor; "
        "  destructor end destructor; end B; "
        "class D : B public dval: int := 3; "
        "  public constructor() base(); end constructor; "
        "  destructor end destructor; end D; "
        "class C public static method Main(): int "
        "  do using var d := new D(); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}
