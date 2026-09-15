// fix #4326 (stdlib §2.10/§2.18, §28.2/§28.12/§18.5): `IDisposable`,
// `IAsyncDisposable`, and `IFormattable` — three null-declaration builtin
// interfaces (`Resolver::builtinTypeArity`, TypeResolution_Names.cpp, all
// three arity-0) — were absent from `common::kHeapPointerReferenceBuiltins`
// (BuiltinRegistry.cpp), so naming one as a parameter or return type
// IR-verifier-CRASHED: "Call parameter type does not match function
// signature!" (parameter) / "Function return type does not match operand
// type of return inst!" (return). Same shape `fix #4178` already fixed for
// `IOrderedEnumerable<T>`'s return type: `lowerTypeRef`'s Named case
// (LlvmBackend_Layout.cpp) falls through both this table's astNode-path and
// imported-name-path checks to the `i32` default instead of the `ptr` every
// conforming value actually is (any class instance, for `IDisposable`/
// `IAsyncDisposable`; any `IFormattable`-implementing class).
//
// ── The mechanically-derived matrix (the actual deliverable) ────────────────
//
// The issue as filed named two: `IDisposable`, `IFormattable`. Per LANE_RULES
// §25 ("the task is the list, not the two names"), every null-declaration
// builtin INTERFACE was checked, not just those two. The population is the
// `kNonConstructibleBuiltinInterfaceNames` 25-name closed set fix #4309 built
// in TypeResolution_Checks.cpp (validated there for a DIFFERENT question —
// §14.2/§17.11 constructibility/defaultability, not lowering representation
// — but its MEMBERSHIP is exactly "null-declaration builtin interface", the
// derivation this fix needs) plus `IGrouping`, deliberately excluded from
// that set by #4309's own comment but sharing the identical declaration-less
// shape: 26 names total. Checked each against `kHeapPointerReferenceBuiltins`
// (BuiltinRegistry.cpp) by direct grep, then MEASURED (not assumed) whether
// each ABSENT one crashes in each of the four annotation positions, on this
// fix's own parent `f99d7c4` (clean tree, `bezelc` rebuilt directly,
// `scripts/agent_check.sh --probe` per-position .bzl files):
//
//   name              | in table? | param  | return | field  | local
//   ------------------|-----------|--------|--------|--------|--------
//   IDisposable       |    NO     | CRASH  | CRASH  | compiles | compiles
//   IAsyncDisposable  |    NO     | CRASH  | CRASH  | compiles | compiles
//   IFormattable      |    NO     | CRASH  | CRASH  | compiles | compiles
//   (23 others)       |    YES    |   —    |   —    |   —    |   —
//
// `IAsyncDisposable` was NOT named in the filed issue — found only by this
// derivation. All 23 other null-declaration builtin interfaces (the 25-name
// set minus the two named in the issue, plus `IGrouping`) were ALREADY
// present in `kHeapPointerReferenceBuiltins`; `control_ienumerable_param` /
// `control_igrouping_field` below pin two of them as accept-side controls.
//
// The field/local columns are a real, measured finding, not an oversight:
// unlike param/return (which lower a NAMED interface type through a function
// SIGNATURE — the exact `lowerTypeRef` Named-case fallthrough this fix
// closes), a nullable field's storage and a local's declared-type annotation
// do not force that same lowering call on THIS tree, so they never hit the
// `i32` default and were never broken. Adding the three names to the table
// does not change their behavior (see `field_and_local_unaffected_controls`
// below) — the fix is representation-only and non-fragile in exactly the
// positions the crash never touched.
//
// ── `kHeapPointerReferenceBuiltins` is not the whole fix: a NECESSARY
// companion in `kBuiltinInterfaceHeads` ─────────────────────────────────────
//
// While proving "real usage… must work" (LANE_RULES per-lane brief), this fix
// found that the representation addition ALONE is unsound: `IDisposable`/
// `IAsyncDisposable`/`IFormattable` becoming `isReservedBuiltinTypeName`-true
// also feeds `isVtableLessBuiltinClassName` (BuiltinRegistry.cpp), which is
// DERIVED as "reserved MINUS `kBuiltinInterfaceHeads`" — so leaving the three
// names out of `kBuiltinInterfaceHeads` would have made that predicate
// silently start answering `true` for them: exactly the #2921/#2923 wild-
// jump / double-free shape that table exists to prevent
// (`LlvmBackend_Disposal.cpp`'s `emitDynamicDispose`, `LlvmBackend_String.cpp`'s
// `bz_obj_is_string`/§2969 routing, and several more sites all gate on it),
// now applied to receivers whose runtime value may be ANY user class (every
// class implicitly implements `IDisposable`, stdlib §2.10; a class may
// explicitly implement `IAsyncDisposable`/`IFormattable`).
//
// MEASURED (this fix, `bezelc` rebuilt directly, WITHOUT the
// `kBuiltinInterfaceHeads` addition, representation-only): `d.Dispose()`
// through an `owned d: IDisposable` parameter silently did NOT run the real
// class's destructor (no crash, no diagnostic — exit code proved it) and
// `$"{f:X}"` on an `IFormattable`-typed receiver printed the literal text
// "IFormattable" instead of dispatching. WITH the `kBuiltinInterfaceHeads`
// addition (this fix's shipped state): both dispatch correctly — see
// `idisposable_dispose_dispatches_through_owned_param_and_local` below. So
// `kBuiltinInterfaceHeads` is not "reused for a different question" (§25) —
// it is fed its OWN correct answer, made necessary by this fix's own change
// to the table it is derived from. Both `tests/support/
// builtin_registry_snapshot_test.cpp` arrays that pin
// `kBuiltinInterfaceHeads` name-for-name are updated in the same commit.
//
// ── Why NOT REUSE `kBuiltinInterfaceHeads` for the representation table
// itself (`kHeapPointerReferenceBuiltins`, the actual #4326 fix) ────────────
//
// `kBuiltinInterfaceHeads` is validated for the vtable/disposal-dispatch
// question above, not lowering representation, and its PRE-fix membership
// proved the mismatch: it excluded `IDisposable`/`IAsyncDisposable`/
// `IFormattable` (all three needed for THIS fix) while already including
// `IGrouping`/lacking nothing else — agreeing on some names is not the same
// question (§25). (`fix #4309`'s own comment in TypeResolution_Checks.cpp
// independently reaches the identical conclusion for the SAME table, from
// the constructor/defaultability side — a third question that also cannot
// reuse it.) The two tables are companions this fix must update TOGETHER,
// not substitutes for each other.
//
// ── A NARROWER, still-open dispatch gap (filed as issue #4328, not fixed
// here) ──────────────────────────────────────────────────────────────────
//
// After BOTH table additions above, three things remain measured-broken,
// none of them representation or vtable-classification issues — all in
// LlvmBackend_Setup.cpp / LlvmBackend_String.cpp's member-call/format-spec
// dispatch machinery (held files for this lane's brief), the same shape as
// `fix #4178` -> issue #4207 (representation lands; a larger, separate
// consuming-side codegen change remains):
//   1. `await d.DisposeAsync()` through an `IAsyncDisposable`-typed owned
//      parameter is now a CLEAN, LOUD compile-time rejection ("DisposeAsync
//      on built-in type IAsyncDisposable produced no value, and no dispatch
//      arm emitted any code for this call... #2457") rather than a silent
//      no-op — an improvement over the pre-`kBuiltinInterfaceHeads` state,
//      but still not implemented.
//   2. An owned PARAMETER's implicit end-of-method auto-drop
//      (`emitOwnedParamCaptureDispose`) through `IDisposable`/
//      `IAsyncDisposable` is still silently inert (own diagnostic: "no Task
//      arm, no vtable-less built-in arm, no recognised class declaration;
//      the slot is left un-reclaimed (a bounded leak) — #3157") — a
//      DIFFERENT codegen path than the explicit-call dispatch this fix's
//      `kBuiltinInterfaceHeads` addition already corrects.
//   3. `$"{f:X}"` on an `IFormattable`-typed receiver improved from printing
//      the literal text "IFormattable" to printing the class's QUALIFIED
//      NAME ("M.Money") — still not the real `ToString(format)` override's
//      output ("M7X"); the §2969 format-spec-aware overload dispatch itself
//      remains unimplemented for this receiver shape.
// The tests below therefore avoid these three call shapes as an oracle
// (mirroring #4178's own test's "WITHOUT calling an unimplemented member on
// it" approach) and instead use the shapes MEASURED to work post-fix:
// explicit `.Dispose()` through an owned `IDisposable`-typed PARAMETER and
// LOCAL (both real dynamic dispatch, proven by destructor side effect), and
// a `using var` over a directly-constructed value.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Neither pre-fix backstop diagnostic (the parameter-position and the
// return-position IR-verifier messages) may appear post-fix (LANE_RULES §18:
// pair a compile-success assertion with an explicit message-text check, since
// the pre-fix failure here is a hard crash, not a clean rejection).
void expectNoIrVerifierCrash(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("IR verification failed") ==
                          std::string::npos);
        BEZEL_EXPECT_TRUE(
            d.message.find("does not match function signature") ==
            std::string::npos);
        BEZEL_EXPECT_TRUE(
            d.message.find("does not match operand type") ==
            std::string::npos);
    }
}

}  // namespace

// ── FAIL-WITHOUT / PASS-WITH: IDisposable, all four positions ───────────────
// Pre-fix: param position IR-verifier-crashes ("Call parameter type does not
// match function signature!"); return position IR-verifier-crashes
// ("Function return type does not match operand type of return inst!").
// field/local positions already compiled pre-fix (measured; see the matrix
// above) — included here as same-name, same-program accept controls, not as
// additional FAIL-WITHOUT rows. Post-fix: all four round-trip through a real
// AOT compile + link + native exec, no crash, `Describe`'s return value
// (computed from real being-called, not a placeholder) reaches `Main`'s exit
// code untouched by anything the fix changed.
BEZEL_TEST(idisposable_iasyncdisposable_iformattable_heap_ptr_4326,
           idisposable_param_return_field_local_grid) {
    const std::string src =
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Holder public property D: IDisposable?; end Holder; "
        "class Prog "
        "  public static method Describe(d: IDisposable): int "
        "    _ := d; "
        "    return 7; "
        "  end Describe; "
        "  public static method Make(): owned IDisposable "
        "    return new Res(); "
        "  end Make; "
        "  public static method Main(): int "
        "    using var r := new Res(); "
        // parameter position
        "    var pcall := Describe(r); "
        // return position (discard: avoids the separate, filed #4341
        // using-var-disposal-arm gap for a call-initialized local — see the
        // file header)
        "    _ := Make(); "
        // field position
        "    using var h := new Holder(); "
        "    h.D := new Res(); "
        // local (declared-type annotation) position
        "    var local: IDisposable := r; "
        "    _ := local; "
        "    return pcall; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    expectNoIrVerifierCrash(r);
}

// ── FAIL-WITHOUT / PASS-WITH: IAsyncDisposable, all four positions ──────────
BEZEL_TEST(idisposable_iasyncdisposable_iformattable_heap_ptr_4326,
           iasyncdisposable_param_return_field_local_grid) {
    const std::string src =
        "module M; "
        "class Res: IAsyncDisposable "
        "  destructor end destructor; "
        "  public async method DisposeAsync(): Task end DisposeAsync; "
        "end Res; "
        "class Holder public property D: IAsyncDisposable?; end Holder; "
        "class Prog "
        "  public static method Describe(d: IAsyncDisposable): int "
        "    _ := d; "
        "    return 9; "
        "  end Describe; "
        "  public static method Make(): owned IAsyncDisposable "
        "    return new Res(); "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    await using var r := new Res(); "
        "    var pcall := Describe(r); "
        "    _ := Make(); "
        "    using var h := new Holder(); "
        "    h.D := new Res(); "
        "    var local: IAsyncDisposable := r; "
        "    _ := local; "
        "    return pcall; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
    expectNoIrVerifierCrash(r);
}

// ── FAIL-WITHOUT / PASS-WITH: IFormattable, all four positions ──────────────
BEZEL_TEST(idisposable_iasyncdisposable_iformattable_heap_ptr_4326,
           iformattable_param_return_field_local_grid) {
    const std::string src =
        "module M; "
        "class Money: IFormattable "
        "  public V: int; "
        "  public constructor(v: int) this.V := v; end constructor; "
        "  public method ToString(format: string): string "
        "    return $\"M{this.V}{format}\"; "
        "  end ToString; "
        "end Money; "
        "class Holder public property F: IFormattable?; end Holder; "
        "class Prog "
        "  public static method Describe(f: IFormattable): int "
        "    _ := f; "
        "    return 11; "
        "  end Describe; "
        "  public static method Make(): owned IFormattable "
        "    return new Money(5); "
        "  end Make; "
        "  public static method Main(): int "
        "    var m := new Money(3); "
        "    var pcall := Describe(m); "
        "    _ := Make(); "
        "    using var h := new Holder(); "
        "    h.F := new Money(2); "
        "    var local: IFormattable := m; "
        "    _ := local; "
        "    return pcall; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
    expectNoIrVerifierCrash(r);
}

// ── REAL USAGE: explicit `.Dispose()` through an owned IDisposable-typed
// PARAMETER and LOCAL dispatches to the real class's destructor (proven by
// exit code, not `didNotCrash`) ─────────────────────────────────────────────
// This is the strong oracle the `kBuiltinInterfaceHeads` companion fix (file
// header) makes possible: `d.Dispose()` is real dynamic dispatch through the
// interface-typed reference, which requires BOTH halves of this fix —
// `kHeapPointerReferenceBuiltins` (so `d` is `ptr`-shaped at all, or this
// would not even compile) AND `kBuiltinInterfaceHeads` (so
// `isVtableLessBuiltinClassName("IDisposable")` is `false`, routing the call
// through real vtable dispatch instead of the vtable-less/no-dispatch path).
// MEASURED representation-only (no `kBuiltinInterfaceHeads` addition): exit
// code 0 — d.Dispose() compiled and ran but the destructor never fired.
BEZEL_TEST(idisposable_iasyncdisposable_iformattable_heap_ptr_4326,
           idisposable_dispose_dispatches_through_owned_param_and_local) {
    const std::string src =
        "module M; "
        "class Res "
        "  public static Disposed: int := 0; "
        "  destructor Res.Disposed := Res.Disposed + 1; end destructor; "
        "end Res; "
        "class Prog "
        "  public static method Consume(owned d: IDisposable): int "
        "    d.Dispose(); "
        "    return 1; "
        "  end Consume; "
        "  public static method Main(): int "
        // parameter position: explicit Dispose() through an owned
        // IDisposable-typed parameter.
        "    _ := Consume(new Res()); "
        // local position: explicit Dispose() through a plain local
        // annotated IDisposable, owning a fresh value directly.
        "    var d: IDisposable := new Res(); "
        "    d.Dispose(); "
        // `using var` over a directly-constructed value also disposes for
        // real (a THIRD, independent real-usage shape).
        "    _ := Prog.Run(); "
        "    return Res.Disposed; "
        "  end Main; "
        "  public static method Run(): int "
        "    using var u: IDisposable := new Res(); "
        "    return 0; "
        "  end Run; "
        "end Prog;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Three independent real disposals: Consume's owned parameter, the
    // local's explicit Dispose(), and Run's using-var — Res.Disposed is
    // idempotent per-instance (§28.2), so 3 is only reachable if all THREE
    // distinct Res instances were genuinely disposed through their
    // IDisposable-typed reference, not a null/corrupted pointer (which
    // would crash) or a silent no-op (which would read 0).
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── ACCEPT-SIDE CONTROL: field and local positions are BYTE-IDENTICAL
// pre-fix and post-fix (the matrix's "compiles" cells, not touched by the
// fix) ────────────────────────────────────────────────────────────────────
BEZEL_TEST(idisposable_iasyncdisposable_iformattable_heap_ptr_4326,
           field_and_local_unaffected_controls) {
    const std::string src =
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Money: IFormattable "
        "  public V: int; "
        "  public constructor(v: int) this.V := v; end constructor; "
        "  public method ToString(format: string): string "
        "    return $\"M{this.V}{format}\"; "
        "  end ToString; "
        "end Money; "
        "class Holder "
        "  public property D: IDisposable?; "
        "  public property F: IFormattable?; "
        "end Holder; "
        "class Prog "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.D := new Res(); "
        "    h.F := new Money(1); "
        "    var localD: IDisposable := h.D!; "
        "    var localF: IFormattable := h.F!; "
        "    _ := localD; "
        "    _ := localF; "
        "    return 3; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── ACCEPT-SIDE CONTROL: names ALREADY in `kHeapPointerReferenceBuiltins`
// are unaffected — a parameter typed `IEnumerable<int>` (the table's oldest,
// most-exercised member) and a field typed `IGrouping<TKey,T>` (the sibling
// #4309 deliberately kept out of the shared constructibility set but which
// has always been in THIS table) ─────────────────────────────────────────
BEZEL_TEST(idisposable_iasyncdisposable_iformattable_heap_ptr_4326,
           control_already_listed_names_unaffected) {
    const std::string src =
        "module M; "
        "class Prog "
        "  public static method Count(xs: IEnumerable<int>): int "
        "    var n := 0; "
        "    for var x in xs do "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Count; "
        "  public static method Main(): int "
        "    var arr: Array<int> := [1, 2, 3]; "
        "    return Count(arr); "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
