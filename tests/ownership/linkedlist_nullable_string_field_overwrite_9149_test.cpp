// Codegen / disposal — an `owned LinkedList<string?>` FIELD reassigned
// repeatedly leaked the displaced list's `string?` elements: the
// drop-on-overwrite arm for an owned collection field (`Emitter::lowerOwnedCollectionFieldStore`,
// src/codegen/LlvmBackend_Setup.cpp) gated the per-element release on
// `!collElem->nullable`, so a `string?` element's +1 was never given back
// (7253 B / 398 allocations over 200 overwrites). The `LinkedList<string>`
// twin of the same program was clean.
//
// Issue #9149.
//
// Spec: §28.7 "Reassigning owned fields and locals — drop-on-overwrite"
// ("Assignment to an owned field or owned local disposes the displaced value
// (derives: K3.11)" — and with overwrite covered, §28.3's claim that "every
// owned value is disposed exactly once on every path" holds without
// qualification), §28.11 "Collection element ownership" *The element slot
// policy* (the runtime-managed-reference row's borrowing-family column is
// "shared references" and the store "stores the reference" — the slot policy
// is a PAIR: the store takes the reference, the collection's free gives it
// back, as #9064 and #9107 implemented it), §3.6 "Derived type properties"
// *Nullable forms* (a nullable type `V?` is read as `V`, nullability
// stripped — so the ELEMENT CATEGORY of a `string?` element is the same
// runtime-managed-reference row as a `string` one's), and §28.9
// "Runtime-managed types" (a `string`'s lifetime is managed by the runtime).
// Spec revision v0.359.
//
// Model: #9107 (the same gate on every other linked-list string-element
// predicate) and #9064 (the same gate on the array-backed family).
//
// ROW BASES. The defect was live on the tree the issue was filed against
// (r30-c1 @ f97d4c0a8 plus #9107): the first row was RED there with the
// issue's own numbers ("7253 byte(s) leaked in 398 allocation(s)"). It is
// GREEN on the current base because `7c5182e2e` ("fix #9100: every
// string-element take/release site of a generic collection reads the same
// element type on both halves", §28.11/§28.7/§3.6, v0.359) replaced that
// arm's `!collElem->nullable` conjunct with
// `isStringElementSlotPaired(collTy, collElem)` — the shared predicate whose
// linked-list reading has no nullability gate. This file is the regression
// row that PINS that: it is RED again the moment the `!nullable` gate (or
// any narrower reading of the element type) comes back, and it is the only
// thing in the tree that notices.
//
//   * field_overwrite_releases_the_displaced_string_elements_... — RED when
//     the drop-on-overwrite arm gates the element release on `!nullable`
//     again (the issue's own defect: the leak).
//   * control_nonnullable_twin_field_overwrite_is_clean_... — the
//     `LinkedList<string>` twin; green on every arm, and RED under a wrong
//     fix that releases the displaced list's elements twice (a
//     "refcount over-release (string)" abort) or narrows the release to the
//     nullable form only.
//
// Every string is built at run time (a folded literal is an RC sentinel on
// which release is a no-op and would hide the leak). runAotChecked runs the
// GENERATED module under ASan and LeakSanitizer at exit, so both a leak (the
// bug) and a double-free / use-after-free flip `r.ran`; stdout pins the
// values (a use-after-free on read-back would corrupt them).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro: an `owned LinkedList<string?>` field reassigned 200
// times. Every replacement list holds one run-time string element AND one
// `null` element — the null element must be SKIPPED by the release, not
// released: the runtime's `bz_llist_release_contents` disposes only a
// non-null element (src/runtime/bezel_runtime_collections.cpp) and
// `bz_string_release` returns on a null pointer
// (src/runtime/bezel_runtime_refcounting.cpp), so a null element is the
// null-safe no-op on both sides. The displaced list must give its string
// elements back exactly once.
const char* kFieldOverwriteNullable =
    "module M; import System;\n"
    "public class Holder\n"
    "  public owned _r: LinkedList<string?> := new LinkedList<string?>();\n"
    "  public method Replace(i: int)\n"
    "    var n := new LinkedList<string?>();\n"
    "    n.Add(\"w\" + i.ToString()); n.Add(null);\n"
    "    _r := n;\n"
    "  end Replace;\n"
    "  public method Cnt(): int return _r.Count; end Cnt;\n"
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

// The `string` twin of the same program (the issue says it is clean). It
// guards the other direction: a fix that gives the displaced list's elements
// back TWICE would abort here with "refcount over-release (string)".
const char* kFieldOverwriteNonNullable =
    "module M; import System;\n"
    "public class Holder\n"
    "  public owned _r: LinkedList<string> := new LinkedList<string>();\n"
    "  public method Replace(i: int)\n"
    "    var n := new LinkedList<string>();\n"
    "    n.Add(\"w\" + i.ToString()); n.Add(\"nn\");\n"
    "    _r := n;\n"
    "  end Replace;\n"
    "  public method Cnt(): int return _r.Count; end Cnt;\n"
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

BEZEL_TEST(ownership_linkedlist_nullable_string_field_overwrite_9149,
           field_overwrite_releases_the_displaced_string_elements_spec_28_7) {
    expectClean(kFieldOverwriteNullable, "400\n");
}

BEZEL_TEST(ownership_linkedlist_nullable_string_field_overwrite_9149,
           control_nonnullable_twin_field_overwrite_is_clean_spec_28_7) {
    expectClean(kFieldOverwriteNonNullable, "400\n");
}
