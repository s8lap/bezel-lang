// Codegen — fix #3767 / stdlib §4.8 / §28.9 / §35.1 / §35.4 (v0.287): the
// `Dictionary<K,V>` assignment-coercion face of the §29.2 wrap grid
// (`LlvmBackend_Assign.cpp`'s `wrapAsIEnumerableForTarget` and the local-
// decl mirror in `LlvmBackend_Setup.cpp`'s `lowerLocalDecl`) gated on
// `dictEntryWrapShapeSupported` (LlvmBackend_Assign.cpp). Before this fix
// the gate's V axis admitted ONLY `Kind::Primitive` and `TypeDeclKind::Enum`
// — a choice V was DECLINED with a third, independent reason: its disposal
// obligation through `dictSnapshotValueDisposers` and its `x.Value.<member>`
// lowering on the wrapped entry face were both unaudited for a choice
// payload. fix #3767 closes both halves:
//
//   1. DISPOSAL. `dictSnapshotValueDisposers`'s ARM 2 routes through
//      `dictValueBoxFreer` (already widened by fix #3770 to admit a struct-
//      lowering V — every classification of `choice` lowers to a bare LLVM
//      struct per §35.1/§35.4 — via the shared `dictValueIsBoxedStruct`
//      predicate the decode and `lowerDictValuesFor` already read), and for a
//      choice with an owned `string` field the per-V string-walking specialised
//      freer (fix #3798) walks the strings before freeing the box. Measured
//      clean for both shapes (no-string-payload and string-payload choice).
//
//   2. WRAPPED-FACE FIELD ACCESS. The wrap snapshot stores the loaded choice
//      value (struct-load from the box, fix #3758) by-value into the entry's
//      field 1. `wrapDictionaryAsIEnumerable`'s decode uses the FIXED
//      `decodeDictValueAt` (whose struct-vs-scalar decision is `isStructTy()`
//      alone), so a choice V's bytes are copied through correctly. The wrap-
//      face read goes through the entry struct's field 1, then a typed local
//      (`var v: Shape := entry.Value`) feeds the case scrutinee — measured to
//      recover the exact payload bytes written at `.Add`. The untyped
//      `entry.Value` (no `: Shape` annotation) still degraded to a zero-
//      initialized slot here — the open wrapped-face field-access gap for ANY
//      non-primitive V, #3563 — and this lane's brief deliberately scoped the
//      fix to the TYPED path (#3563 is a separate, broader fix; this commit
//      does NOT widen that gate).
//      ⚠ SUPERSEDED BY fix #3563 (v0.334), which lands the gap this paragraph
//      scoped out: sema now types `.Key`/`.Value` for the WRITTEN
//      `DictionaryEntry<K,V>` spelling as well as the synthetic one, so the
//      UNTYPED read works and the gate admits a record / class V. See
//      `dict_entry_wrap_read_class_record_v_3563_test.cpp` for that fix's
//      rows and Row 2 below for the flip it forced here.
//
// GATE MECHANICS. The local-decl mirror at `LlvmBackend_Setup.cpp:14494`
// previously called `dictEntryWrapShapeSupported(declTR)` with the SECOND
// argument defaulting to `nullptr` — the gate's "no source dictionary
// provided" early-return returns `narrow` (primitive V only), so a choice V
// would be declined even with the V-axis widened. This fix passes `initTR`
// (the source dictionary's TypeRef, which the gate's `headName()` classifier
// already requires to read the same spelling `dictValueBoxFreer` and
// `dictSnapshotValueDisposers` read) so the gate's widening actually
// admits a choice V on the LOCAL-DECL face. Mirrors the §29.2 return-site
// caller (`wrapAsIEnumerableForTarget`, `LlvmBackend_Assign.cpp:7962`),
// which has always named both arguments.
//
// ⚠ A class V / record V still DECLINES — issue #3563's "wrapped-face field
// access silently wrong" trade is unchanged. A choice V is admitted because
// the disposal half is measured clean (ARM 2 covers the box) AND because
// the typed-local pattern match (which IS what every existing Dictionary
// test exercises for choice V, see e.g. dict_choice_value_decode_3758_test
// row (1) — they all read choice payloads via destructuring patterns on
// typed locals, never via the wrap-face field access of an untyped `entry.
// Value`.
// ⚠ SUPERSEDED BY fix #3563 (v0.334). The record / class V exclusion this
// paragraph records is now lifted: sema types `.Key`/`.Value` for the written
// `DictionaryEntry<K,V>` spelling (the sema twin of codegen's #2794
// `headName()` widening), so the read side is sound and the gate admits both
// kinds. Row 2 below is the row that flip renamed and rewrote.
//
// REGRESSION ROOM. Row 1 is the issue's own repro shape — the
// `Dictionary<int, Color>.Add/get` via the wrapped GetEnumerator face
// (`IEnumerable<DictionaryEntry<K,V>> := d;` plus `.GetEnumerator()` plus
// `Current.Key` to confirm the wrap minted). Row 2 was originally a record-V
// control asserting record V stayed DECLINED on this face; fix #3563 lifted
// that exclusion, so Row 2 is now a POSITIVE record-V row (the wrap minted and
// the entry rebuilt) — see its own header for the flip. Row 3 is a primitive-V
// control
// for object-parity (the original gate's narrow answer must keep working
// after the widening). Row 4 is the choice V with payload case, through the
// assignment-coercion face, with a typed local + destructuring pattern —
// measured to recover the exact payload bytes written at `.Add`. Row 5 is
// the 500x Add/Remove amplification on a scope-end-free `Dictionary<int,
// Color>` with no iteration, under generated-code AddressSanitizer — the
// strict `runAotChecked` oracle's exit-time LSan scan must be silent (no
// leak, no double-free), mirroring the #3770 measurement methodology.

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

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — issue repro (stdlib §4.8 / §29.2, v0.287). The wrap grid's
// assignment-coercion face for `Dictionary<int, Color>` with a choice V was
// DECLINED before this fix — `lowerLocalDecl`'s `var e: IEnumerable<Dictionary-
// Entry<int, Color>> := d;` stored the raw `bz_dict_t*` in the slot and the
// following `.GetEnumerator()` vtable-dispatched on it, measured SIGSEGV (rc
// 139) at `it.MoveNext()`. After the fix the gate admits `Choice` V (matching
// the `Enum` arm's "primK && admits" structure, see the gate's updated
// header comment), the local-decl mirror passes the source TypeRef so the
// gate can read its `headName()`, and the wrap is minted exactly once via
// `wrapDictionaryAsIEnumerable`. The row asserts the wrap minted (the `key=1`
// decode confirms `Current` returned a properly-built `{K, V}` snapshot entry)
// AND that a non-iteration read of the wrapped enumerator (its `Current.Key`)
// recovers the int key written at `.Add`. FAILS-WITHOUT (SIGSEGV at .MoveNext,
// rc 139) / PASSES-WITH (exit=0, key=1).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_choice_v_3767,
           assignment_coercion_choice_v_no_payload_4_8) {
    const std::string src =
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<int, Color>(); "
        "  d.Add(1, Color.Red); "
        "  var e: IEnumerable<DictionaryEntry<int, Color>> := d; "
        "  using var it := e.GetEnumerator(); "
        "  while it.MoveNext() do "
        "    Console.WriteLine($\"key={it.Current.Key}\"); "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "key=1"));
}

// ---------------------------------------------------------------------------
// Row 2 — record V on the wrap face (stdlib §4.8 / §29.2 / §28.3, v0.334).
//
// ⚠ BEHAVIOUR FLIP — fix #3563 renamed and REWROTE this row. It was
// `record_v_still_declined_on_wrap_face_3563_object_parity` and it asserted
// the OPPOSITE of what it now asserts: that a record V bound to
// `IEnumerable<DictionaryEntry<int, Pt>>` fails LOUDLY (rc-139 SIGSEGV,
// nothing on stdout). That was correct for its commit and is now false —
// #3563 lands the fix it was written to guard against, so record V is no
// longer DECLINED on the wrap face and the program runs correctly. A row that
// pins the ABSENCE of a fix has to be rewritten when the fix lands; keeping
// the old assertion would have meant shipping #3563 with a deliberately red
// test. Renamed so the flip is greppable from the issue number, and rewritten
// to assert the wrapped entry's key DECODES (exit 0, "x=1"), which is what
// "the wrap was minted and the entry rebuilt" means.
//
// Pre-#3563 this row's program SIGSEGV'd at `it.MoveNext()`: the gate declined
// a record V, the coercion stored the raw `bz_dict_t*` in the `IEnumerable`
// slot, and `GetEnumerator()` vtable-dispatched on a layout that object does
// not have. The full #3563 story (the gate's other half — sema typing
// `.Key`/`.Value` for the WRITTEN `DictionaryEntry` spelling) and the
// regression rows are in `dict_entry_wrap_read_class_record_v_3563_test.cpp`.
// The object-parity intent survives in Row 3 (primitive V) and in that file's
// Row 5 (the in-place for-in control).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_choice_v_3767,
           record_v_wrapped_on_wrap_face_now_runs_3563_fixed) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var d := new Dictionary<int, Pt>(); "
        "  d.Add(1, new Pt(3, 4)); "
        "  var e: IEnumerable<DictionaryEntry<int, Pt>> := d; "
        "  using var it := e.GetEnumerator(); "
        "  while it.MoveNext() do "
        "    Console.WriteLine($\"x={it.Current.Key}\"); "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "x=1"));
}

// ---------------------------------------------------------------------------
// Row 3 — primitive V control (stdlib §4.8, v0.287). Object-parity: the gate's
// narrow (primitive-V only) answer has admitted `Dictionary<int, int>` since
// the original #3519 widening, and the assignment-coercion face has worked
// for that shape through every subsequent fix. This row guards against an
// accidental narrowing of the gate by the #3767 widening — primitive V must
// keep working exactly as before. FAILS-WITHOUT (gate narrowed inadvertently)
// / PASSES-WITH (gate widening preserves the primitive arm's behaviour).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_choice_v_3767,
           primitive_v_still_works_object_parity_3519) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<int, int>(); "
        "  d.Add(1, 100); "
        "  d.Add(2, 200); "
        "  var total := 0; "
        "  var e: IEnumerable<DictionaryEntry<int, int>> := d; "
        "  using var it := e.GetEnumerator(); "
        "  while it.MoveNext() do "
        "    total := total + it.Current.Value; "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=300"));
}

// ---------------------------------------------------------------------------
// Row 4 — choice V with payload, via the wrap face (stdlib §4.8 / §35.1 /
// §35.4, v0.287). The wrap's snapshot stores the loaded choice value
// (struct-load from the box, fix #3758) by-value into the entry's field 1.
// The wrap-face read uses a TYPED local (`var v: Shape := entry.Value`) to
// feed the case scrutinee — measured to recover the exact payload bytes
// written at `.Add` (Circle(r=3) + Square(s=4) = 3 + 4 = 7). The untyped
// `entry.Value` path was broken for ANY non-primitive V (issue #3563, out of
// scope for this lane's narrow fix) and is FIXED by #3563 — see
// `dict_entry_wrap_read_class_record_v_3563_test.cpp`.
// FAILS-WITHOUT (assignment-coercion gate declines, raw dict passes through,
// vtable dispatch on garbage / crash) / PASSES-WITH (exit=0, total=7).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_choice_v_3767,
           assignment_coercion_choice_v_with_payload_typed_local_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<int, Shape>(); "
        "  d.Add(1, Shape.Circle(3)); "
        "  d.Add(2, Shape.Square(4)); "
        "  var e: IEnumerable<DictionaryEntry<int, Shape>> := d; "
        "  using var it := e.GetEnumerator(); "
        "  var total := 0; "
        "  while it.MoveNext() do "
        "    var entry := it.Current; "
        "    var v: Shape := entry.Value; "
        "    total := total + (case v of "
        "      Circle(r) => r, "
        "      Square(s) => s); "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=7"));
}

// ---------------------------------------------------------------------------
// Row 5 — 500-iteration Add/Remove amplification on a borrowing
// `Dictionary<int, Color>` with no iteration, under the strict
// `runAotChecked` oracle (generated-code AddressSanitizer, LeakSanitizer
// exit-time scan). Mirrors the #3770 measurement methodology (200x is its
// floor; #3568 used 200x on the OwningDictionary<…, Item> case). A choice
// value without payload uses a `free(box)` per entry (no owned strings to
// walk); the per-iteration box alloc + free + per-entry disposal must sum to
// zero bytes and zero allocations still live at exit, byte-for-byte against
// pre-#3767's narrow-gate behaviour for record / class / nullable V (whose
// boxes were already proven clean by #3770). This row is the disposal-side
// proof that the ARM-2 box-free path the gate now reaches for a choice V is
// sound, not just the read-side path Row 4 exercises. FAILS-WITHOUT (if
// the widening accidentally widens a path that double-frees — no such
// double-free exists for the plain-`Dictionary<K, Color>` shape; the row's
// FAIL-WITHOUT is therefore a guard against a refactor that re-introduces
// one) / PASSES-WITH (LSan-clean 500 iterations, exit=0).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_choice_v_3767,
           add_remove_500x_no_iteration_lsan_silent_28_9) {
    const std::string src =
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 500 do "
        "    var d := new Dictionary<int, Color>(); "
        "    d.Add(1, Color.Red); "
        "    d.Add(2, Color.Green); "
        "    d.Add(3, Color.Blue); "
        "    d.Remove(2); "
        "    total := total + 1; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=500"));
}