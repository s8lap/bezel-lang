// Codegen / disposal — a GENERIC `Dictionary<string, T>` whose value type `T`
// carries a `string` leaked that string when the dictionary was disposed.
//
// The value is heap-boxed (stdlib §4.8's value encoding) and the box is the
// collection's own storage. §28.11's element slot policy gives the BORROWING
// family's slot for a value-type element the COPY policy — the store row's
// "copies the value in" — and the copy the collection holds carries the
// value's runtime-managed references (§28.9: a `string` is runtime-managed;
// the runtime reclaims it once the last holder is gone, and the collection's
// copy is one of its holders). The collection's Dispose row is "no user-visible
// work" for a copy slot — no destructor call — but the copy's runtime-managed
// reference is still the collection's to give back. The per-V box freer that
// does that is chosen by `Emitter::dictValueBoxFreer`
// (src/codegen/LlvmBackend_Disposal.cpp), and its choice was keyed on the
// value type's `declaration`:
//
//     const sema::TypeRef* valTy = dictTy->genericArgs[1];
//     if (valTy->kind == Named && valTy->declaration) { /* per-V walk */ }
//     return valueBoxFreeThunk();       // a plain free(box)
//
// Inside a MONOMORPHIZED generic body the dictionary's value type is still the
// PLACEHOLDER type parameter (`T`) — a Named TypeRef with NO `declaration` —
// so the per-V string walk never fired and the box went back with a bare
// `free(box)`, which releases no string at all. The matching TAKE half was
// gated the same way (`boxValueNeedsStringRetain`, fix #3974), so the box took
// no `+1` on the value's string fields either: NEITHER half of the pair
// fired (issue #9197).
//
// The fix resolves the value type through the ACTIVE substitution before both
// halves read it — the same `resolveTypeParamRef` resolution
// `elementPolicyFor` uses for the element slot (fix #4596, §28.11 / §14.1)
// and the owned-FIELD path uses for its element (fix #816) — so a generic V
// reaches the per-V string-walking freer its non-generic twin reaches, and the
// take half agrees with it (§14.1: a generic body is monomorphized per
// instantiation; the value's shape is fixed at the instantiation, not at the
// generic declaration).
//
// WHY THE ROWS BELOW ARE THE CHOICE-VALUE SHAPE. The leak a generic value box
// can be blamed for is the value's OWN `+1`: a box that holds a `+1` it never
// gives back. A RECORD value's string field does not carry one on its own —
// the field's `+1` is the SOURCE's, and the source (a by-value record
// argument) never releases it: that is a separate, already-filed defect
// (#5865 / #6153), and it leaks the SAME bytes in the generic and the
// non-generic shape alike (MEASURED, this lane: the issue's own repro leaks
// 7600 B / 400 allocations with the fix as well as without it — the
// non-generic twin leaks exactly the same). A CHOICE value's payload DOES
// carry its own `+1` (the case constructor's field store retains it, §9.7 /
// §35.4), so the box's copy of it is an owned `+1` — and the box's plain
// `free(box)` leaks it. That is the leak this fix removes, and it is the
// same gate the issue names: the per-V box free's string walk needs a
// `declaration`, and a bare type parameter has none.
//
// The row that covers the issue's own literal repro (a generic RECORD value)
// is the LAST one: it runs under the lenient oracle (generated-code ASan
// only) because the repro's residual leak is the source-side defect above,
// not this one. It pins that the take half's new `+1` on a generic record
// value is balanced by the release half — a retain without its matching
// release would show up as a "refcount over-release" abort.
//
// Spec: §28.9 (runtime-managed types), §28.11 (the element slot policy — the
// copy-slot store row and the Dispose row), §14.1 (generic instantiation and
// its site), §9.7 / §35.4 (a choice's payload), stdlib §4.8 (the dictionary's
// value encoding). Spec revision v0.359.
//
// The two RED rows are RED on the base (LeakSanitizer) and GREEN with the fix;
// the two control rows are GREEN on both, by construction. The loop is 200
// iterations so the leak is 400 allocations, not a one-shot that could hide
// under the sanitizer's noise floor.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// THE REGRESSION ROW (the scope-end LOCAL path). A GENERIC
// `Dictionary<string, T>` whose T is a choice with a `string` payload: the
// value is boxed (stdlib §4.8) and the box's payload carries the case
// constructor's own `+1` on that string.
//
// RED on the base: "SUMMARY: AddressSanitizer: 7600 byte(s) leaked in 400
// allocation(s)" — the per-value string the plain `free(box)` never released,
// reported by LeakSanitizer at the dictionary's scope end. GREEN with the
// fix: the generic path reaches `bz$chValBoxFree$Shape`, the per-V
// string-walking freer its non-generic twin already reached.
const char* kGenericLocal =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class Holder<T> "
    "  public method Put(v: T): int "
    "    var d := new Dictionary<string, T>(); "
    "    d[\"a\"] := v; "
    "    return d.Count; "
    "  end Put; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var n := 0; "
    "    while i < 200 do "
    "      using var h := new Holder<Shape>(); "
    "      n := n + h.Put(Shape.Named(\"p\" + (i + 1000).ToString())); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

// THE SAME SHAPE ON THE `owned` FIELD PATH (#9183's path): the dictionary is
// the holder's own field, so the table goes back at the HOLDER's disposal.
//
// RED on the base: the same 7600 B / 400 allocations — the field path derives
// the value disposer from the same `dictValueBoxFreer`, so the same bare `T`
// kept it on the plain thunk.
const char* kGenericField =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class Holder<T> "
    "  private owned _d: Dictionary<string, T> := new Dictionary<string, T>(); "
    "  public method Put(v: T) => _d[\"a\"] := v; "
    "  public method Count(): int => _d.Count; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var n := 0; "
    "    while i < 200 do "
    "      using var h := new Holder<Shape>(); "
    "      h.Put(Shape.Named(\"p\" + (i + 1000).ToString())); "
    "      n := n + h.Count(); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

// CONTROL 1 — the issue's own contrast: the SAME dictionary with a CONCRETE
// value type already reaches the per-V string-walking freer
// (`bz$chValBoxFree$Shape`), so it is clean. This control is green on the base
// and pins that the fix did not change the non-generic path.
const char* kNongenericLocal =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class Holder "
    "  public method Put(v: Shape): int "
    "    var d := new Dictionary<string, Shape>(); "
    "    d[\"a\"] := v; "
    "    return d.Count; "
    "  end Put; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var n := 0; "
    "    while i < 200 do "
    "      using var h := new Holder(); "
    "      n := n + h.Put(Shape.Named(\"p\" + (i + 1000).ToString())); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

// CONTROL 2 — the ISSUE'S OWN LITERAL REPRO: a generic
// `Dictionary<string, T>` whose T is a RECORD with a `string` field, with the
// value passed in by value. The fix gives this path the per-V string-walking
// freer AND the take half's matching `+1` (both halves now fire where
// neither did), so the box's copy is a balanced owner.
//
// The row runs under the LENIENT oracle (generated-code ASan, no
// LeakSanitizer) on purpose: the repro's residual leak is the SOURCE's `+1`
// (a by-value record argument is never released — #5865 / #6153, filed
// separately), and it leaks the same bytes in the non-generic twin, so it is
// not this fix's to remove. What this row pins is that the newly-taken `+1`
// is given back: an unbalanced retain would abort the run with "refcount
// over-release (string)", and a use-after-free would trip ASan.
const char* kIssueRepro =
    "module M; "
    "public record Entry(Name: string, Expires: int); "
    "public class Holder<T> "
    "  public method Put(v: T): int "
    "    var d := new Dictionary<string, T>(); "
    "    d[\"a\"] := v; "
    "    return d.Count; "
    "  end Put; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var n := 0; "
    "    while i < 200 do "
    "      using var h := new Holder<Entry>(); "
    "      n := n + h.Put(new Entry(\"p\" + (i + 1000).ToString(), 10)); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

}  // namespace

// THE REGRESSION ROW: the generic dictionary's value box releases the value's
// own string at the dictionary's scope end.
BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           generic_dictionary_value_box_releases_the_values_own_string) {
    const JitResult r = runAotChecked(kGenericLocal);
    expectClean(r, 0, "200\n");
}

// The same shape on the `owned` FIELD path: the table goes back at the
// HOLDER's disposal.
BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           generic_owned_field_dictionary_value_box_releases_the_values_string) {
    const JitResult r = runAotChecked(kGenericField);
    expectClean(r, 0, "200\n");
}

// CONTROL 1: the concrete-value-type twin is clean already and stays clean.
BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           control_concrete_value_type_dictionary_is_already_clean) {
    const JitResult r = runAotChecked(kNongenericLocal);
    expectClean(r, 0, "200\n");
}

// CONTROL 2: the issue's own literal repro runs without a use-after-free and
// without an unbalanced `+1`.
BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           control_the_issue_repro_runs_without_a_use_after_free) {
    const JitResult r = runAotCheckedLenient(kIssueRepro);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}
