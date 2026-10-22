// Codegen — issue #3708 / stdlib §4.8 / §29.2 / §28.9 (v0.291): REFUTED, with
// a locking regression test.
//
// #3708 claimed: "the LOCAL-DECL face of the §29.2 Dictionary wrap still uses
// the v0.283 primitive-only gate. The shapes #3568 widened on the return
// face work there but SEGV (wild jump) when the same value is bound as a
// typed local" — citing `var e: IEnumerable<DictionaryEntry<int, Color>> :=
// d;` (Color an ENUM) as the repro, with a stated fix shape of "pass
// srcDictTy from initTR at the local-decl mint so both faces share
// dictSnapshotValueDisposers' gate".
//
// VERIFIED ON HEAD (f19de07 + this run's ancestry): the claim was TRUE when
// #3708 was filed (against the review of `b4b5c0f`, fix #3568), but a
// DIFFERENT, later commit already supplies exactly the recommended fix as an
// incidental side effect. `4fdb89c` (fix #3767, landed after #3708 was
// filed) touches `LlvmBackend_Setup.cpp:14494`'s call
//     dictEntryWrapShapeSupported(declTR)         // pre-#3767: srcDictTy
//                                                  // defaults null, gate
//                                                  // answers v0.283 only
// to
//     dictEntryWrapShapeSupported(declTR, initTR)  // #3767: gate sees the
//                                                   // source dictionary,
//                                                   // widens per #3568
// — #3767's own commit body was written for the CHOICE-V axis, but passing
// `initTR` widens EVERY axis the gate now supports, including the Enum-V,
// Enum-K and nullable-V shapes #3708 names. So on HEAD, #3708's exact repro
// no longer SIGSEGVs; it already runs the WIDENED gate.
//
// MEASURED (agent_check --probe --asan, this lane): the issue's own repro
// shape (`Dictionary<int, Color>` local-decl, Color an enum) prints
// "keys=3 total=4", rc 0, clean under ASan/LSan — not the wild-jump SIGSEGV
// the issue reports. Enum-K, nullable-V and (200x-looped, LSan) string-V and
// nullable-V local-decl rows are likewise clean. This file pins that result
// so a future edit to `LlvmBackend_Setup.cpp`'s Dictionary local-decl arm (or
// to `dictEntryWrapShapeSupported`) cannot silently narrow the gate back to
// the v0.283 answer without failing a test — Rows 1-3 FAIL-WITHOUT (measured
// by reverting `LlvmBackend_Setup.cpp:14494` to the pre-#3767 one-argument
// call) and PASS-WITH (current HEAD).
//
// WHY THE MISSING `dictSnapshotValueDisposers` THREADING (the issue's stated
// "fix shape") IS NOT ITSELF A BUG ON THIS FACE. The local-decl mint
// (`LlvmBackend_Setup.cpp`, the `isDictTypeName(initTR) &&
// dictEntryWrapShapeSupported(declTR, initTR)` arm) still calls
// `wrapDictionaryAsIEnumerable(srcDict, declTR)` with NO snapshot value
// disposer, and frees the source (only for an unbound `new`, which is empty
// by construction — there is no dict-literal-with-entries spelling that is
// an `ast::NewExpr`) via the one-argument `freeDictSourceNow(srcDict)`. Per
// `wrapDictionaryAsIEnumerable`'s own header comment
// (`LlvmBackend_impl.hpp`): "The default (null) is the correct answer for
// every BORROWING face — the explicit `.GetEnumerator()` call and the
// local-decl mint both leave the source dictionary alive and owning its
// values, so the snapshot must not touch them." A named-variable local-decl
// source (`var d := new Dictionary<...>(); ...; var e: IEnumerable<...> :=
// d;`, this file's shape) is exactly that BORROWING face: `d` is never freed
// by this coercion, so any value the snapshot decode ALIASED (a co-owned
// `string`, per #429) or LOADED OUT of a box (a nullable payload) is still
// live and owned by `d` alone; `d`'s own later scope-end disposal (LIFO
// after `e`'s, since `e` is declared after `d`) is the only release that
// obligation ever needs. Threading a deferred disposer into the LOCAL-DECL
// wrap's slot-1 Dispose here would DOUBLE-RELEASE `d`'s still-live string
// values / boxes. Rows 4-5 (200x, `runAotChecked`) pin that the current
// no-disposer local-decl wrap is LSan-clean for exactly the shapes where a
// disposer would be wrong to add.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// (Never put a C++ `//` comment inside these literals: it is lexed as Bezel
// source and comments out the rest of the program. Bezel's own comment forms
// are `//`, `/* ... */` and `///` -- spec §1.5, which is exhaustive.)

const char* const kEnumColor =
    "public enum Color  Red := 1, Green := 2, Blue := 3;  end Color; ";

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — THE ISSUE'S OWN REPRO. `Dictionary<int, Color>` (Color an ENUM,
// matching #3568's return-face row (E)) bound to `IEnumerable<DictionaryEntry
// <int, Color>>` at a LOCAL-DECL, `d` a plain named borrowing source (NOT
// `new` — the wrap must not free `d`). Per §10 (enums are integer-backed) and
// stdlib §4.8.
//
// FAILS-WITHOUT (measured: reverting Setup.cpp:14494 to the pre-#3767
// `dictEntryWrapShapeSupported(declTR)` one-argument call reintroduces the
// v0.283-only gate; the coercion then falls through to
// `wrapUserClassAsIEnumerable` over the raw `bz_dict_t*`, and `.MoveNext()`
// vtable-dispatches on a layout that isn't a user-class object — SIGSEGV,
// wild jump, rc 139) / PASSES-WITH (exit 0, "keys=3 total=4").
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_localdecl_widened_3708,
           local_decl_borrowed_enum_v_matches_issue_repro_10) {
    const std::string src =
        std::string("module M; ") + kEnumColor +
        "public class Main "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, Color>(); "
        "    d[1] := Color.Red; "
        "    d[2] := Color.Blue; "
        "    var e: IEnumerable<DictionaryEntry<int, Color>> := d; "
        "    var keys := 0; "
        "    var total := 0; "
        "    for var x in e do "
        "      keys := keys + x.Key; "
        "      if x.Value = Color.Blue then total := total + 3; end if; "
        "      if x.Value = Color.Red then total := total + 1; end if; "
        "    end for; "
        "    Console.WriteLine($\"keys={keys} total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "keys=3 total=4"));
}

// ---------------------------------------------------------------------------
// Row 2 — Enum KEY, local-decl, borrowed source. `Dictionary<Color, int>`.
// Mirrors #3568's return-face row (F) on the LOCAL-DECL face. An enum key
// takes `bz_dict_keys_gen`'s generic-key table (loads by value, frees the
// malloc'd raw-bytes copy) — no ownership crosses into the snapshot, so this
// row is also a control on the K axis of the widened gate.
//
// FAILS-WITHOUT (same one-line revert as Row 1: gate declines, SIGSEGV) /
// PASSES-WITH (exit 0, "reds=1 total=30").
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_localdecl_widened_3708,
           local_decl_borrowed_enum_k_10) {
    const std::string src =
        std::string("module M; ") + kEnumColor +
        "public class Main "
        "  public static method Main(): int "
        "    var d := new Dictionary<Color, int>(); "
        "    d[Color.Red] := 10; "
        "    d[Color.Blue] := 20; "
        "    var e: IEnumerable<DictionaryEntry<Color, int>> := d; "
        "    var total := 0; "
        "    var reds := 0; "
        "    for var x in e do "
        "      total := total + x.Value; "
        "      if x.Key = Color.Red then reds := reds + 1; end if; "
        "    end for; "
        "    Console.WriteLine($\"reds={reds} total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "reds=1 total=30"));
}

// ---------------------------------------------------------------------------
// Row 3 — NULLABLE primitive VALUE, local-decl, borrowed source.
// `Dictionary<int, int?>`. Mirrors #3568's return-face row (D). `int?` is
// `Kind::Primitive` with `->nullable` true — the axis the return face's gate
// comment calls out as needing an explicit `->nullable` check rather than a
// bare `Kind` test.
//
// FAILS-WITHOUT (same one-line revert: gate declines a nullable V exactly
// like an enum V, SIGSEGV) / PASSES-WITH (exit 0, "keys=6 present=2").
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_localdecl_widened_3708,
           local_decl_borrowed_nullable_v_539) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, int?>(); "
        "    d[1] := 10; "
        "    d[2] := null; "
        "    d[3] := 30; "
        "    var e: IEnumerable<DictionaryEntry<int, int?>> := d; "
        "    var keys := 0; "
        "    var present := 0; "
        "    for var x in e do "
        "      keys := keys + x.Key; "
        "      if x.Value <> null then present := present + 1; end if; "
        "    end for; "
        "    Console.WriteLine($\"keys={keys} present={present}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "keys=6 present=2"));
}

// ---------------------------------------------------------------------------
// Row 4 — DISPOSAL-SIDE PIN. 200x, a co-owned (#429) `string` VALUE,
// local-decl, borrowed source, under `runAotChecked` (generated-code
// AddressSanitizer + exit-time LeakSanitizer). Every iteration builds a
// fresh `Dictionary<int, string>` (4 interpolated -- non-literal -- string
// values, so a missed release is visible to LSan, matching #3568's row (A)
// methodology), wraps it as a LOCAL-DECL borrow (never frees the source
// dictionary through this coercion), and iterates. Pins the design fact in
// this file's header: the local-decl BORROWING face must NOT install a value
// disposer on the wrap's slot-1 Dispose for a co-owned string value, because
// `d` itself still owns it and releases it at `d`'s own (later) scope end.
// FAILS-WITHOUT would mean a future change wrongly threads a disposer here
// and double-releases `d`'s strings (LSan would instead report a
// double-free/heap-use-after-free, not a leak) or wrongly withholds `d`'s own
// release (a leak) -- this row's job is to catch either regression.
// PASSES-WITH: exit 0, "total=1600", LSan silent.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_localdecl_widened_3708,
           local_decl_borrowed_string_v_200x_lsan_clean_429) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 200 do "
        "      var d := new Dictionary<int, string>(); "
        "      var j := 0; "
        "      while j < 4 do "
        "        d[j] := $\"a{j}\"; "
        "        j := j + 1; "
        "      end while; "
        "      var e: IEnumerable<DictionaryEntry<int, string>> := d; "
        "      for var x in e do "
        "        total := total + x.Value.Length; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1600"));
}

// ---------------------------------------------------------------------------
// Row 5 — DISPOSAL-SIDE PIN. 200x, a nullable-VALUE-TYPE box (the #539
// tagged-struct payload the decode `CreateLoad`s out of the table's box),
// local-decl, borrowed source, under `runAotChecked`. Mirrors Row 4 for the
// OTHER disposal shape `dictSnapshotValueDisposers` distinguishes (ARM 2,
// "loaded out of a box", vs. Row 4's ARM 3, "aliased the slot word"): here
// the snapshot COPIES the {tag,value} struct bytes by value, so nothing
// heap-owned crosses into the snapshot at all and `d`'s own table (still
// alive, still holding its own boxes) is the sole owner throughout. PASSES-
// WITH: exit 0, "keys=1200 present=400", LSan silent.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_wrap_localdecl_widened_3708,
           local_decl_borrowed_nullable_v_200x_lsan_clean_539) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var keys := 0; "
        "    var present := 0; "
        "    while i < 200 do "
        "      var d := new Dictionary<int, int?>(); "
        "      d[1] := 10; "
        "      d[2] := null; "
        "      d[3] := 30; "
        "      var e: IEnumerable<DictionaryEntry<int, int?>> := d; "
        "      for var x in e do "
        "        keys := keys + x.Key; "
        "        if x.Value <> null then present := present + 1; end if; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"keys={keys} present={present}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "keys=1200 present=400"));
}
