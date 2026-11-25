// Codegen / disposal — an `owned Set<string?>` FIELD reassigned repeatedly
// leaked the displaced set's `string?` elements: the drop-on-overwrite arm
// for an owned collection field (`Emitter::lowerOwnedCollectionFieldStore`,
// src/codegen/LlvmBackend_Setup.cpp) and the holder's own field disposal
// (`Emitter::emitOwnedFieldDisposals`, src/codegen/LlvmBackend_Disposal.cpp)
// decided the per-element release from a list of collection heads that did
// not name the SET, so the displaced set's `string?` element's +1 was never
// given back (7290 B / 400 allocations over 200 overwrites). The `Set<string>`
// twin of the same program was clean.
//
// Issue #9263.
//
// THE REPRESENTATION (measured, and the reason the fix lands where it does).
// A `Set<T>` is DICT-backed (`bz_dict_t*`, its elements are KEYS released by
// `bz_set_string_keys_release`) only when its element is generic-dict-eligible
// — and `genericBackingType` (src/codegen/LlvmBackend_Calls3.cpp) declines a
// NULLABLE element (its `isString` test requires a non-nullable string, and
// every other eligibility arm is `!ty->nullable`-gated). So `Set<string>` is
// dict-backed and `Set<string?>` is ARRAY-backed (`bz_array_t*`), and the
// dict-backed Set's key-release machinery never sees a `string?` at all. The
// array-backed Set's element slot is a PAIRED §28.11 slot: its insert (the
// array family's `Add`, `arrIsStringElem` in LlvmBackend_Calls.cpp) takes the
// +1 for a `string?` element exactly as for a `string` one, and its `Remove`
// / `Clear` / free give it back — so the FREE must carry the per-element
// disposer, which the two field sites above did not derive for the SET head.
//
// Spec: §28.7 "Reassigning owned fields and locals — drop-on-overwrite"
// ("Assignment to an owned field or owned local disposes the displaced value
// (derives: K3.11)"), §28.11 "Collection element ownership" *The element slot
// policy* (the runtime-managed-reference row's borrowing-family column is
// "shared references" and the store "stores the reference" — the slot policy
// is a PAIR: the store takes the reference, the collection's free gives it
// back, as #9064 and #9107 implemented it), §3.6 "Derived type properties"
// *Nullable forms* (a nullable type `V?` is read as `V`, nullability
// stripped — so the ELEMENT CATEGORY of a `string?` element is the same
// runtime-managed-reference row as a `string` one's), §28.9 "Runtime-managed
// types" (a `string`'s lifetime is managed by the runtime; its reference is
// dropped exactly once) and §28.3 "Ownership" (every owned value is disposed
// exactly once on every path). Spec revision v0.359.
//
// ROW BASES. Every defect row below is RED on the tree the issue was filed
// against (r30-c1 @ 76cc32fa2) with the issue's own numbers:
//
//   * field_overwrite_releases_the_displaced_string_elements_... — RED on the
//     base with "SUMMARY: AddressSanitizer: 7290 byte(s) leaked in 400
//     allocation(s)" — the issue's own figure (one string per overwrite: its
//     32-byte header + its buffer). It is GREEN with the fix.
//   * field_disposal_releases_the_field_elements_... — RED on the base with
//     the same 7290 B / 400 allocations, from the OTHER site: one assignment
//     of a 200-element set, so nothing is displaced and the leak is the
//     holder's own disposal of the field. It is GREEN with the fix.
//   * control_nonnullable_twin_field_overwrite_is_clean_... — the
//     `Set<string>` twin; green on every arm, and RED under a wrong fix that
//     gives the displaced set's elements back TWICE (a "refcount over-release
//     (string)" abort) or narrows the release to the nullable form only.
//
// Every string is built at run time (a folded literal is an RC sentinel on
// which release is a no-op and would hide the leak). runAotChecked runs the
// GENERATED module under ASan and LeakSanitizer at exit, so both a leak (the
// bug) and a double-free / use-after-free flip `r.ran`; stdout pins the
// values (a use-after-free on read-back would corrupt them).
//
// NOT IN THIS FILE: the scope-end of a `Set<string?>` LOCAL, its `Remove` and
// its `Clear` are CLEAN on the base for a run-time-string element (measured),
// so they pin nothing and belong in the conformance corpus, not in gtest
// (lane contract §7). Written with a LOCAL string element instead, all three
// are RED on the base — but for issue #8661's defect (the local's own +1 is
// orphaned by `StringEscapeWalker` when a collection stores it), which is a
// SEPARATE open issue this commit does not touch.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro: an `owned Set<string?>` field reassigned 200 times.
// Every replacement set holds one run-time string element AND one `null`
// element — the null element must be SKIPPED by the release, not released:
// the runtime's `bz_array_free` disposes only a non-null element
// (src/runtime/bezel_runtime_collections.cpp) and `bz_string_release` returns
// on a null pointer (src/runtime/bezel_runtime_refcounting.cpp), so a null
// element is the null-safe no-op on both sides. The displaced set must give
// its string elements back exactly once.
const char* kFieldOverwriteNullable =
    "module M; import System;\n"
    "public class Holder\n"
    "  public owned _s: Set<string?> := new Set<string?>();\n"
    "  public method Replace(i: int)\n"
    "    var n := new Set<string?>();\n"
    "    n.Add(\"w\" + i.ToString()); n.Add(null);\n"
    "    _s := n;\n"
    "  end Replace;\n"
    "  public method Cnt(): int return _s.Count; end Cnt;\n"
    "end Holder;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var h := new Holder();\n"
    "    for var i in 0..<200 do\n"
    "      h.Replace(i);\n"
    "      tot := tot + h.Cnt();\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The OTHER site: ONE assignment of a set holding 200 string elements, so
// nothing is ever displaced and the leak is the HOLDER'S disposal of the
// field (`emitOwnedFieldDisposals`). The field's element disposer must be
// derived for the SET head there too.
const char* kFieldDisposal =
    "module M; import System;\n"
    "public class Holder\n"
    "  public owned _s: Set<string?> := new Set<string?>();\n"
    "  public method Fill()\n"
    "    var n := new Set<string?>();\n"
    "    for var i in 0..<200 do\n"
    "      n.Add(\"w\" + i.ToString());\n"
    "    end;\n"
    "    n.Add(null);\n"
    "    _s := n;\n"
    "  end Fill;\n"
    "  public method Cnt(): int return _s.Count; end Cnt;\n"
    "end Holder;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var h := new Holder();\n"
    "    h.Fill();\n"
    "    Console.WriteLine(h.Cnt());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The `string` twin of the issue's repro (the issue says it is clean). Its
// element is a RUN-TIME string, so the twin's release path is exercised: a
// fix that gives the displaced set's elements back TWICE aborts here with
// "bezel: fatal: refcount over-release (string)" (rc 134) — the same
// over-release the rejected commit feb164d20 for #9064 produced.
const char* kFieldOverwriteNonNullable =
    "module M; import System;\n"
    "public class Holder\n"
    "  public owned _s: Set<string> := new Set<string>();\n"
    "  public method Replace(i: int)\n"
    "    var n := new Set<string>();\n"
    "    n.Add(\"nn\" + i.ToString());\n"
    "    _s := n;\n"
    "  end Replace;\n"
    "  public method Cnt(): int return _s.Count; end Cnt;\n"
    "end Holder;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var h := new Holder();\n"
    "    for var i in 0..<200 do\n"
    "      h.Replace(i);\n"
    "      tot := tot + h.Cnt();\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

void expectClean(const char* src, const char* out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak (the bug) and no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(ownership_set_nullable_string_field_overwrite_9263,
           field_overwrite_releases_the_displaced_string_elements_spec_28_7) {
    expectClean(kFieldOverwriteNullable, "400\n");
}

BEZEL_TEST(ownership_set_nullable_string_field_overwrite_9263,
           field_disposal_releases_the_field_elements_spec_28_7) {
    expectClean(kFieldDisposal, "201\n");
}

BEZEL_TEST(ownership_set_nullable_string_field_overwrite_9263,
           control_nonnullable_twin_field_overwrite_is_clean_spec_28_7) {
    expectClean(kFieldOverwriteNonNullable, "200\n");
}
