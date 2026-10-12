// Codegen / crash — fix #3563 (§29.2 / §9.4 / stdlib §4.8, v0.334): the
// WRAPPED `IEnumerable<DictionaryEntry<K,V>>` ENTRY FACE, read-side half.
//
// THE DEFECT, and the two independent causes a triage lane's hypothesis named
// only one of. `var e: IEnumerable<DictionaryEntry<string, V>> := dict;`
// followed by an enumeration crashed (SIGSEGV, rc 139) for a class or record
// `V`. Two things had to be true for that to happen, and fixing either alone
// is not a fix:
//
//   (A) `dictEntryWrapShapeSupported` (`LlvmBackend_Assign.cpp`) DECLINED a
//       record / class V, so the coercion stored the RAW `bz_dict_t*` in the
//       `IEnumerable` slot and the following `GetEnumerator()` dispatched
//       through a vtable that object does not have — an indirect call at a
//       null/garbage pc. MEASURED pre-fix: SIGSEGV at pc 0x0, and it fires on
//       `.Key` ALONE (reading `it.Current.Key`, never touching `.Value`), so
//       the crash is at the coercion face, not at the value read.
//
//   (B) sema typed `.Key`/`.Value` only when the receiver's TypeRef carried
//       the SYNTHETIC `importedName="DictionaryEntry"` tag
//       (`ExprTypecheck_Calls.cpp`). A receiver whose `DictionaryEntry<string,
//       V>` arrives through the WRITTEN annotation — which is what a
//       `IEnumerable<DictionaryEntry<...>>` local always carries — has an
//       astNode and an EMPTY importedName, so it was missed. Codegen's own
//       `#548` DictionaryEntry arm had already been widened to `headName()`
//       for exactly that reason (fix #2794); the sema half never followed.
//       With no recorded type on the `entry.Value` node, any expression that
//       needs that node's own type to go further finds `exprType` null:
//       `entry.Value.<member>` directly, or `var v := entry.Value;` (an
//       UNTYPED `:=` infers from that same node) followed by `v.<member>`.
//       Codegen's CG8 field read then missed, `fieldAddr` returned null, and
//       the #2961 arm dropped the store — a silent zero for a record V and a
//       null receiver for a class V.
//
// MEASURED, per half, on this build:
//   gate widened alone  → (A) fixed, (B) still open: `total=0` where the
//                         answer is 42/3 — the exact "quiet wrong answer" the
//                         gate's own ⚠ note warned against, now reached.
//   sema fix alone      → (B) fixed, (A) still open: the program still
//                         SIGSEGVs at the coercion, because no wrap is minted.
//   both                → correct, no #2961 warning, LSan-clean.
//
// The class-V method-call spelling additionally crashes rather than degrading,
// because a class method call on a null receiver is an indirect call through a
// null vtable — so (B) alone for a class V is a SIGSEGV even with the wrap in
// place.
//
// DISPOSAL. The gate widening rests on `dictSnapshotValueDisposers` already
// covering both shapes, which is verified here rather than assumed: Row 6
// (500x class V) and Row 7 (200x record V carrying an OWNED `string` field)
// run under the strict `runAotChecked` oracle, whose exit-time LSan scan must
// be silent — no leak, no double-free. Row 8 pins the ownership-correct RETURN
// face (the class instance passed in as a PARAMETER, so the wrapper aliases a
// live caller-owned object rather than a callee-scope-ending local).
//
// Rows 5 and 7 are object-parity CONTROLS for the synthetic-`importedName`
// spelling the sema change must keep admitting (the in-place `for var entry
// in dict` face never had a type for the tag-only path to miss).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The class V every class-V row shares: one owned field, one method that
// READS that field (so a wrong receiver is observable, not merely a wrong
// pointer). A method call on a class value is the sharpest probe of (B): with
// a zero-initialized local it dispatches through a null vtable.
const char* const kClassV =
    "class V(n: int) public "
    "  method Get(): int return n; end Get; "
    "end V; ";

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — THE ISSUE'S OWN REPRO, verbatim shape (stdlib §4.8 / §29.2,
// v0.334): a class V, `Dictionary<string, V>` coerced at a local decl to
// `IEnumerable<DictionaryEntry<string, V>>`, enumerated with for-in, and the
// entry's VALUE read through a method call. §29.2's wrap face; §11 for the
// class; §28.3 for `v`'s ownership (the dictionary slot BORROWS it — the
// borrowing family does not own its values, §28.11 — so `v` stays the owner
// for the whole iteration).
//
// FAILS-WITHOUT (both halves reverted): SIGSEGV, rc 139, nothing on stdout —
// cause (A), the raw `bz_dict_t*` vtable dispatch at `GetEnumerator()`.
// PASSES-WITH: exit 0, "total=42".
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_read_class_record_v_3563,
           class_v_wrapped_forin_value_method_call_4_8) {
    const std::string src =
        "module M; " + std::string(kClassV) +
        "class C public static method Main(): int "
        "  var v := new V(42); "
        "  var dict := new Dictionary<string, V>(); "
        "  dict.Add(\"key\", v); "
        "  var e: IEnumerable<DictionaryEntry<string, V>> := dict; "
        "  var total := 0; "
        "  for var entry in e do "
        "    var result := entry.Value; "
        "    total := total + result.Get(); "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=42"));
}

// ---------------------------------------------------------------------------
// Row 2 — cause (B) in its DIRECT spelling, record V (§9 records / stdlib §4.8
// / §29.2, v0.334): `entry.Value.x` with no intermediate local. §9.4 makes
// `Key`/`Value` read-only positional fields; §14.1 makes `x` a plain field
// read on the record the entry carries.
//
// This is the row the old gate comment measured as "silently computes 0". With
// ONLY cause (A) fixed it prints "total=0"; with only the sema fix it
// SIGSEGVs; the two together give the right answer.
//
// FAILS-WITHOUT (both halves reverted): SIGSEGV rc 139 (cause (A)); with the
// gate alone, "total=0" (cause (B)). / PASSES-WITH: exit 0, "total=3".
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_read_class_record_v_3563,
           record_v_wrapped_direct_value_field_9_4) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var dict := new Dictionary<string, Pt>(); "
        "  dict.Add(\"key\", new Pt(3, 4)); "
        "  var e: IEnumerable<DictionaryEntry<string, Pt>> := dict; "
        "  var total := 0; "
        "  for var entry in e do "
        "    total := total + entry.Value.x; "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=3"));
}

// ---------------------------------------------------------------------------
// Row 3 — cause (B) in its INDIRECT spelling: the UNTYPED `:=` local
// (stdlib §4.8 / §5, v0.334). `var result := entry.Value;` has no annotation
// to fall back on, so its type is inferred from the very node sema failed to
// type — the same null `exprType` Row 2 hits, one step further from the
// entry. The TYPED spelling `var result: Pt := entry.Value;` works even
// pre-fix (the annotation supplies the type), which is exactly why the bug
// survived: every Dictionary test that reads a record/class payload reads it
// through a typed local.
//
// FAILS-WITHOUT: SIGSEGV rc 139 (cause (A)); gate-only, "total=0" (cause B).
// PASSES-WITH: exit 0, "total=3".
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_read_class_record_v_3563,
           record_v_wrapped_untyped_local_then_field_4_8) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var dict := new Dictionary<string, Pt>(); "
        "  dict.Add(\"key\", new Pt(3, 4)); "
        "  var e: IEnumerable<DictionaryEntry<string, Pt>> := dict; "
        "  var total := 0; "
        "  for var entry in e do "
        "    var result := entry.Value; "
        "    total := total + result.x; "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=3"));
}

// ---------------------------------------------------------------------------
// Row 4 — the explicit-`.GetEnumerator()` spelling of the same face (stdlib
// §4.8 / §4.12, v0.334). `it.Current` is typed by the IEnumerator arm above
// the DictionaryEntry arm, so `it.Current` itself has a type; it is the
// `.Value` on THAT node that sema missed. Reading the payload straight off
// `it.Current.Value` (no local at all) is the third spelling of cause (B) and
// the one a §4.12-shaped user writes.
//
// FAILS-WITHOUT: SIGSEGV rc 139; gate-only, "total=0". / PASSES-WITH: exit 0,
// "total=3".
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_read_class_record_v_3563,
           record_v_wrapped_current_value_direct_4_12) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var dict := new Dictionary<string, Pt>(); "
        "  dict.Add(\"key\", new Pt(3, 4)); "
        "  var e: IEnumerable<DictionaryEntry<string, Pt>> := dict; "
        "  using var it := e.GetEnumerator(); "
        "  var total := 0; "
        "  while it.MoveNext() do "
        "    total := total + it.Current.Value.x; "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=3"));
}

// ---------------------------------------------------------------------------
// Row 5 — OBJECT-PARITY CONTROL for the SYNTHETIC `importedName` spelling
// (stdlib §4.8, v0.334). The sema change widens `importedName ==
// "DictionaryEntry"` to `headName() == "DictionaryEntry"`; `headName()`
// returns `importedName` verbatim when there is no astNode, so the in-place
// `for var entry in dict` face — whose loop-variable TypeRef is the SYNTHETIC
// one, carrying no written annotation — must behave byte-identically. This row
// passes both before and after the fix by construction: it is the control that
// makes the widening's "strict superset" claim checkable rather than argued.
//
// PASSES-WITH (and PASSES-WITHOUT): exit 0, "total=42".
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_read_class_record_v_3563,
           class_v_inplace_forin_object_parity_4_8) {
    const std::string src =
        "module M; " + std::string(kClassV) +
        "class C public static method Main(): int "
        "  var v := new V(42); "
        "  var dict := new Dictionary<string, V>(); "
        "  dict.Add(\"key\", v); "
        "  var total := 0; "
        "  for var entry in dict do "
        "    var result := entry.Value; "
        "    total := total + result.Get(); "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=42"));
}

// ---------------------------------------------------------------------------
// Row 6 — DISPOSAL, class V on the widened local-decl face, 500x under the
// strict `runAotChecked` oracle (stdlib §4.8 / §28.9 / §28.11, v0.334). The
// 500-iteration floor is the point: a one-shot repro cannot distinguish "the
// wrapper frees the borrowed instance" from "the wrapper never frees
// anything". Each iteration wraps a dictionary that BORROWS one class
// instance, disposes both, and reads the instance through the wrapped entry —
// a double-free shows as a crash, a missing free as an LSan leak, and neither
// is observable at one iteration. The instance is bound to a named `v` (not a
// fresh `.Add` temporary) so §28.3 ownership is unambiguous: `v` is the sole
// owner and the wrap must never dispose it.
//
// PASSES-WITH: exit 0, "total=124750" (= 0+1+…+499), LSan silent.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_read_class_record_v_3563,
           class_v_wrapped_500x_lsan_silent_28_9) {
    const std::string src =
        "module M; " + std::string(kClassV) +
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var total := 0; "
        "  while i < 500 do "
        "    var v := new V(i); "
        "    var dict := new Dictionary<string, V>(); "
        "    dict.Add(\"key\", v); "
        "    var e: IEnumerable<DictionaryEntry<string, V>> := dict; "
        "    for var entry in e do "
        "      var r := entry.Value; "
        "      total := total + r.Get(); "
        "    end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=124750"));
}

// ---------------------------------------------------------------------------
// Row 7 — DISPOSAL, record V carrying an OWNED `string` field, 200x, both the
// typed and the untyped read (stdlib §4.8 / §28.9 / §28.11, v0.334). A record
// V is the ONE wrapped shape whose value is BOXED on `.Add` and `CreateLoad`ed
// OUT of the box by the snapshot decode, so the snapshot holds its own bytes
// while the box dies at the source free — the split `dictSnapshotValueDisposers`
// ARM 2 exists for. A record whose field is an owned `string` adds the
// string-walking per-V freer (#3798) on top of the box free, so a widening that
// got the box wrong shows here as either a leak (freed twice / not at all) or
// a use-after-free (freed too early). Both read spellings run in one program so
// the row covers the typed local and the untyped one.
//
// PASSES-WITH: exit 0, "total=1400" (= 200 × 7), LSan silent.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_read_class_record_v_3563,
           record_v_owned_string_200x_lsan_silent_28_9) {
    const std::string src =
        "module M; "
        "record Named(Tag: string, N: int); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var total := 0; "
        "  var typed := 0; "
        "  while i < 200 do "
        "    var dict := new Dictionary<string, Named>(); "
        "    dict.Add(\"key\", new Named(\"tagvalue\", 7)); "
        "    var e: IEnumerable<DictionaryEntry<string, Named>> := dict; "
        "    for var entry in e do "
        "      var a: Named := entry.Value; "
        "      var b := entry.Value; "
        "      typed := typed + a.N + b.N; "
        "    end; "
        "    total := total + typed; "
        "    typed := 0; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=2800"));
}

// ---------------------------------------------------------------------------
// Row 8 — DISPOSAL, the RETURN coercion face with a class V whose instance is
// OWNED BY THE CALLER (stdlib §4.8 / §28.9 / §29.2, v0.334). `Make2` takes the
// instance as a parameter and returns the wrapped enumerable, so the snapshot
// ALIASES a caller-owned object that is still live for the whole iteration —
// the shape `dictSnapshotValueDisposers`' ARM 1 (DEFERRED) and the
// `srcOwnedByCallee` source free have to get right. The instance is bound with
// `using var` so its lifetime ends after the loop body, never during it.
//
// This row is also the discriminator that isolates cause (A) from the
// pre-existing §28.3 fresh-temporary leak it sits next to: passing the
// instance in (rather than `new`-ing it inside `Make`) removes the fresh-
// temporary question entirely, so a leak here would be the wrap's.
//
// FAILS-WITHOUT (both halves reverted): SIGSEGV rc 139 — the gate declines a
// class V, so `Make2` returns the raw `bz_dict_t*` and the caller's for-in
// vtable-dispatches on it (cause (A), on the RETURN face rather than the
// local-decl one). / PASSES-WITH: exit 0, "total=12600", LSan silent.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_read_class_record_v_3563,
           class_v_return_face_caller_owned_300x_lsan_silent_28_9) {
    const std::string src =
        "module M; " + std::string(kClassV) +
        "class Factory public static method Make2(v: V): "
        "IEnumerable<DictionaryEntry<string, V>> "
        "  var dict := new Dictionary<string, V>(); "
        "  dict.Add(\"key\", v); "
        "  return dict; "
        "end Make2; end Factory; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var total := 0; "
        "  while i < 300 do "
        "    using var t := new V(42); "
        "    var e := Factory.Make2(t); "
        "    for var entry in e do "
        "      var r := entry.Value; "
        "      total := total + r.Get(); "
        "    end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=12600"));
}