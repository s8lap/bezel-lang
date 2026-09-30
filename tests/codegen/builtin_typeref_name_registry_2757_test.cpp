// Codegen — `Emitter::isBuiltinTypeRefName` derives its whole reserved-name set
// from the shared builtin registry (issue #2757, spec v0.240).
//
// §7.1 (Non-nullable and nullable types): "A plain reference type is
// non-nullable; a `?` suffix produces its nullable form". §7.2 (Nullable value
// types): "A value type wrapped in `?` is an optional". Neither says anything
// about which BUILTIN the name denotes — so a `?` annotation on any reserved
// builtin name must lower to that builtin's own representation, never to the
// {i8, i32} placeholder that lowerTypeRef's #568 branch reserves for a nullable
// bare generic type PARAMETER.
//
// Root cause (#2757). A declaration-less, single-part Named TypeRef is one of
// exactly two things: a reserved builtin, or a type parameter. There is no
// positive registry of type-PARAMETER names to test against (a parameter is
// defined by the ABSENCE of a declaration), so three codegen sites ask the
// question NEGATIVELY through `isBuiltinTypeRefName` — lowerTypeRef's #568
// nullable-type-parameter branch, `isBareTypeParamRef` (#2743), and the
// generic-argument INFERENCE binding in LlvmBackend_Calls2.cpp (#1162). That
// makes the name set's COMPLETENESS load-bearing: a missing name is silently
// read as a type parameter. `isBuiltinTypeRefName` kept a hand-maintained
// duplicate of the set — 59 names where `common::isHeapPointerReferenceBuiltin`
// carried 65 — and it drifted, so `Stopwatch?` took the placeholder and passed a
// `bz_stopwatch_t*` in a 32-bit payload to `bz_stopwatch_is_running`
// (IR-verify failure on a legal program). That was the THIRD instance of the
// same miss-a-list defect, after #1390 (serializer pointer truncation) and
// #1406 (SortedDictionary as i32) — the two whose fix created the registry.
//
// #2743 patched it by consulting the registry IN ADDITION to the local list;
// #2757 deletes the list, so the set now lives once, in
// `common::isReservedBuiltinTypeName` (src/common/BuiltinRegistry.cpp), pinned
// by tests/support/builtin_registry_snapshot_test.cpp. That is a
// behaviour-preserving change — every case below passes on the commit before it
// too. What it buys is that each test here now pins ONE registry entry: drop a
// name from either registry table and the matching case regresses, in the same
// direction the original defect did (the `Stopwatch` mutation was demonstrated
// mechanically for the inference case below).
//
// Coverage is chosen for the names #2743's regression test does NOT reach:
// registry names absent from the deleted list (SortedSet, SortedDictionary), a
// non-heap delta (ArraySpan, Guid), and the THIRD consumer — inference binding —
// which no test covered at all (#2743's `Box<Stopwatch>` uses an EXPLICIT type
// argument, so it never reaches the inference path).
//
// The last two cases are ACCEPT-CONTROLS for what the predicate exists to
// protect: a genuine user-chosen type parameter must still be classified as one,
// and a USER type whose name collides with a builtin must still win (it carries
// a declaration, so the predicate is never consulted for it).

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A mis-classified builtin carries its heap pointer in a 32-bit payload, so
// force generated-code ASan: a truncated pointer that survives IR verification
// must not pass silently. -999 (a value no test below returns) marks a
// crash/leak.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── stdlib §4.18 `SortedSet<T>`: a registry heap-pointer builtin that the
//    deleted hand-list did NOT carry. `o!.Count` reaches a ptr-taking runtime
//    entry, so a {i8, i32} lowering truncates the bz_dict_t*. §7.1.
BEZEL_TEST(codegen_builtin_typeref_name_registry_2757, sorted_set_nullable_annotation_7_1) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var s := new SortedSet<int>(); "
        "  var o: SortedSet<int>? := s; "
        "  s.Add(3); "
        "  s.Add(9); "
        "  if o = null then return 1; end; "
        "  if o!.Count = 2 then return 7; end; "
        "  return 2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(e, 7);
}

// ── stdlib §4.17 `SortedDictionary<K, V>`: the OTHER registry name whose
//    absence from the hand-list was the original #1406 defect. §7.1.
BEZEL_TEST(codegen_builtin_typeref_name_registry_2757, sorted_dictionary_nullable_annotation_7_1) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var d := new SortedDictionary<int, int>(); "
        "  var o: SortedDictionary<int, int>? := d; "
        "  d[1] := 4; "
        "  d[2] := 3; "
        "  if o = null then return 1; end; "
        "  if o![1] + o![2] = 7 then return 7; end; "
        "  return 2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(e, 7);
}

// ── stdlib §4.15 `ArraySpan<T>`: a NON-heap reserved name — the half of the set
//    that is not in `isHeapPointerReferenceBuiltin` and so lives in the
//    registry's `kNonHeapReservedBuiltins` table. Its {ptr, i32, i32} record has
//    no null tag either way, so the annotation is read through `!` rather than
//    compared against null; the point is that the span's SOURCE pointer survives
//    the `?` annotation instead of being crushed into an i32 payload. §7.1.
BEZEL_TEST(codegen_builtin_typeref_name_registry_2757, array_span_nullable_annotation_7_1) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); "
        "  a.Add(20); "
        "  a.Add(30); "
        "  var sp: ArraySpan<int>? := a.AsSpan(1, 2); "
        "  if sp!.Length = 2 and sp![0] = 20 and sp![1] = 30 then return 7; end; "
        "  return 2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(e, 7);
}

// ── stdlib §2.6 `Guid`: a non-heap reserved name that IS intercepted upstream
//    by `isNullableValueTy` (it is one of the five names `valueBuiltinBoxName
//    Str` boxes), so `Guid?` is the tagged {i8, %bz.guid} optional of §7.2 —
//    the control showing that a reserved name in the non-heap table does not
//    disturb the nullable-VALUE path that runs before the #568 branch.
BEZEL_TEST(codegen_builtin_typeref_name_registry_2757, guid_nullable_annotation_7_2) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  var g: Guid? := null; "
        "  if g <> null then return 1; end; "
        "  g := Guid.NewGuid(); "
        "  if g = null then return 2; end; "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(e, 7);
}

// ── the THIRD consumer, previously uncovered: generic-argument INFERENCE
//    binding (LlvmBackend_Calls2.cpp, `if (cn.empty() || !isBuiltinTypeRefName
//    (cn)) return;`). `Echo(s)` infers `T = Stopwatch` from the ARGUMENT — an
//    unmissable difference from #2743's `Box<Stopwatch>`, which supplies the
//    type argument explicitly and so never reaches this path. A name the
//    predicate does not know is skipped rather than bound, leaving T unbound:
//    the call then lowers T to the i32 fallback while the monomorphized body
//    uses `ptr`. §14.2 / §7.1.
//    (Mechanically demonstrated: deleting "Stopwatch" from the registry's
//    kHeapPointerReferenceBuiltins turns this case into a compile failure.)
BEZEL_TEST(codegen_builtin_typeref_name_registry_2757, inferred_type_arg_binds_heap_builtin_14_2) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class C "
        "  public static method Echo<T>(v: T): T where T: class "
        "    return v; "
        "  end Echo; "
        "  public static method Main(): int "
        "    using var s := new Stopwatch(); "
        "    s.Start(); "
        "    var e := Echo(s); "
        "    if e.IsRunning then return 7; end; "
        "    return 2; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(e, 7);
}

// ── ACCEPT-CONTROL 1: the case the predicate EXISTS to protect. A genuine
//    user-chosen type parameter `T` must still be classified as a parameter, so
//    `T?` keeps the #568 lowering — the tagged optional at T = int, and a plain
//    nullable reference at T = a class. Widening the reserved-name set is only
//    safe while this stays true. §7.10 / §14.1.
BEZEL_TEST(codegen_builtin_typeref_name_registry_2757, bare_type_param_still_a_parameter_7_10) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class Res public n: int := 4; end Res; "
        "class Box<T> "
        "  private _v: T? := null; "
        "  public method Set(v: T) _v := v; end Set; "
        "  public method Get(): T? return _v; end Get; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var bi := new Box<int>(); "
        "  if bi.Get() <> null then return 1; end; "
        "  bi.Set(3); "
        "  var gi := bi.Get(); "
        "  if gi = null then return 2; end; "
        "  using var br := new Box<Res>(); "
        "  if br.Get() <> null then return 3; end; "
        "  using var r := new Res(); "
        "  br.Set(r); "
        "  var gr := br.Get(); "
        "  if gr = null then return 4; end; "
        "  return gi! + gr!.n; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(e, 7);
}

// ── ACCEPT-CONTROL 2: a USER type whose name collides with a reserved builtin.
//    Its TypeRef carries a `declaration`, so every one of the three consumers
//    bails before the name test and the user's own class wins — the reserved
//    set must not be able to hijack it. §7.1.
BEZEL_TEST(codegen_builtin_typeref_name_registry_2757, user_type_shadowing_builtin_name_7_1) {
    int e = runChecked(
        "module M; import System.Diagnostics; "
        "class Stopwatch "
        "  public N: int := 5; "
        "  public constructor() end; "
        "end Stopwatch; "
        "class C public static method Main(): int "
        "  using var s := new Stopwatch(); "
        "  var o: Stopwatch? := s; "
        "  if o = null then return 1; end; "
        "  return o!.N + 2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(e, 7);
}
