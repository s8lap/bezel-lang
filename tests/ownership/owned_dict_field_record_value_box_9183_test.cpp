// Codegen / disposal — an `owned` `Dictionary` FIELD whose value type is a
// RECORD leaked the record's heap box (8 B per stored value) at the holder's
// disposal. The FIELD disposal path frees the table without the value disposer
// that frees a boxed record value; the scope-end LOCAL path installs it
// (`buildCollectionTempDisposal` → `dictValueBoxFreer`), the FIELD path did
// not. Issue #9183 — the record-value twin of #6945(1) (the `string` value).
//
// Spec: §28.9 "Runtime-managed types" (the compiler-inserted free of a
// collection releases the instance's own allocation, and a boxed value held in
// a value slot is storage the collection holds, so it goes back with it),
// §28.11 "Collection element ownership" *The element slot policy* (the
// borrowing family's value slot for a record value is a COPY slot — "copies the
// value in" — and the copy's storage is the collection's to release), §3.2 /
// §9 (a record is a value type: it carries no disposal obligation of its own,
// only memory to free). Stdlib §4.8 (the value encoding: a record is too wide
// for the dictionary's scalar value slot, so it is heap-boxed). Spec revision
// v0.359.
//
// `elementPolicyFor` classifies a record / choice / nullable-record value as a
// VALUE (a value type has no disposal obligation of its own), so its policy
// row carries NO disposer thunk: the field path called `bz_dict_free(val,
// nullptr)` and the box was never freed. The fix installs `dictValueBoxFreer`
// — the SAME helper the local path installs — as the field path's fallback.
//
// Every row below is RED on the base (LSan) and GREEN with the fix. The
// `assertNoLeak` leg is what makes the rows fail: a leak is reported by
// LeakSanitizer at the holder's disposal, and `runAotChecked` (the AOT oracle
// with the generated-code ASan leg) reports the sanitizer report as a failed
// run. The loop is 200 iterations so the leak is 200 allocations, not a
// one-shot that could hide under the sanitizer's noise.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

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

// The issue's literal repro, amplified 200x. The issue's own program is one
// iteration; the loop makes the leak 200 allocations (1600 B) instead of one
// 8-byte box, so the row cannot pass on a sanitizer's noise floor.
//
// The issue's program prints the stored record's field, so the row also reads
// the data the fix handles.
const char* kIssueRepro =
    "module M; "
    "public record Entry(Value: int, Expires: int); "
    "public class Holder "
    "  private owned _d: Dictionary<string, Entry> := new Dictionary<string, Entry>(); "
    "  public method Put() => _d[\"a\"] := new Entry(1, 10); "
    "  public method Get(): int => _d[\"a\"].Expires; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    while i < 200 do "
    "      using var h := new Holder(); "
    "      h.Put(); "
    "      Console.WriteLine(h.Get()); "
    "      i := i + 1; "
    "    end; "
    "  end Main; "
    "end Program; ";

// The CHOICE-value sibling of the same site: a choice also lowers to a bare
// LLVM struct, so it is stored BOXED exactly as a record is (the value
// encoding, stdlib §4.8) and its box goes back with the table.
const char* kChoiceValue =
    "module M; "
    "public choice Shape Circle(r: int), Square; end Shape; "
    "public class Holder "
    "  private owned _d: Dictionary<string, Shape> := new Dictionary<string, Shape>(); "
    "  public method Put() => _d[\"a\"] := Shape.Circle(7); "
    "  public method Count(): int => _d.Count; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var n := 0; "
    "    while i < 200 do "
    "      using var h := new Holder(); "
    "      h.Put(); "
    "      n := n + h.Count(); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

// The NULLABLE-RECORD-value sibling: a nullable value type lowers to the
// tagged `{i8, T}` wrapper, which is box-freed too (stdlib §4.8 / §7.2).
const char* kNullableRecordValue =
    "module M; "
    "public record Entry(Value: int, Expires: int); "
    "public class Holder "
    "  private owned _d: Dictionary<string, Entry?> := new Dictionary<string, Entry?>(); "
    "  public method Put() => _d[\"a\"] := new Entry(1, 10); "
    "  public method Count(): int => _d.Count; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var n := 0; "
    "    while i < 200 do "
    "      using var h := new Holder(); "
    "      h.Put(); "
    "      n := n + h.Count(); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

// The GENERIC holder: `Dictionary<string, T>` at T = a record. The field's
// declared value type is the type PARAMETER, and the box freer is derived
// from the substituted element type, so the generic axis of the same site is
// covered too.
const char* kGenericHolder =
    "module M; "
    "public record Entry(Value: int, Expires: int); "
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
    "      using var h := new Holder<Entry>(); "
    "      h.Put(new Entry(1, 10)); "
    "      n := n + h.Count(); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

// CONTROL 1 — the issue's own contrast: the SAME dictionary as a LOCAL is
// clean (its scope-end path decides the disposal through
// `buildCollectionTempDisposal`, which installs the record-value box freer).
// This control is green on the base and pins that the fix did not change the
// local path. It is red under a plausible wrong fix: one that installs the
// value-box freer at the local site as well (the local path already has it),
// which would free the same box twice.
const char* kControlLocalRecord =
    "module M; "
    "public record Entry(Value: int, Expires: int); "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var n := 0; "
    "    while i < 200 do "
    "      var d := new Dictionary<string, Entry>(); "
    "      d[\"a\"] := new Entry(1, 10); "
    "      n := n + d.Count; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

// CONTROL 2 — the OWNING family is unchanged: `OwningDictionary<string, V>`
// owns its class values (they are disposed through the element policy row),
// and no value box is involved. This control is green on the base and is red
// under a plausible wrong fix: one that installs the value-box freer for the
// owning family too, which would free the class instance's storage a second
// time.
const char* kControlOwningDictionary =
    "module M; "
    "public class Thing "
    "  public constructor() end constructor; "
    "end Thing; "
    "public class Holder "
    "  private owned _d: OwningDictionary<string, Thing> := new OwningDictionary<string, Thing>(); "
    "  public method Put() => _d[\"a\"] := new Thing(); "
    "  public method Count(): int => _d.Count; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var n := 0; "
    "    while i < 200 do "
    "      using var h := new Holder(); "
    "      h.Put(); "
    "      n := n + h.Count(); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(n); "
    "  end Main; "
    "end Program; ";

}  // namespace

// THE REGRESSION ROW: the issue's literal repro, amplified 200x.
//
// RED on the base: "SUMMARY: AddressSanitizer: 1600 byte(s) leaked in 200
// allocation(s)" — 8 B per stored record value, reported by LeakSanitizer at
// the holder's disposal. GREEN with the fix.
BEZEL_TEST(ownership_owned_dict_field_record_value_box_9183,
           issue_repro_200_iterations_reclaims_the_record_value_box) {
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += "10\n";
    const JitResult r = runAotChecked(kIssueRepro);
    expectClean(r, 0, expected);
}

// The CHOICE-value sibling of the same site: a choice is stored boxed exactly
// as a record is, so its box is the table's to release.
//
// RED on the base: 2400 B / 200 allocations (a choice's box is 12 B).
BEZEL_TEST(ownership_owned_dict_field_record_value_box_9183,
           choice_value_box_is_reclaimed_at_the_holders_disposal) {
    const JitResult r = runAotChecked(kChoiceValue);
    expectClean(r, 0, "200\n");
}

// The NULLABLE-RECORD-value sibling: the tagged `{i8, T}` wrapper is
// box-freed too.
//
// RED on the base: 2400 B / 200 allocations.
BEZEL_TEST(ownership_owned_dict_field_record_value_box_9183,
           nullable_record_value_box_is_reclaimed_at_the_holders_disposal) {
    const JitResult r = runAotChecked(kNullableRecordValue);
    expectClean(r, 0, "200\n");
}

// The GENERIC holder: the field's declared value type is the type PARAMETER,
// and the box freer is derived from the substituted element type.
//
// RED on the base: 1600 B / 200 allocations.
BEZEL_TEST(ownership_owned_dict_field_record_value_box_9183,
           generic_holders_record_value_box_is_reclaimed) {
    const JitResult r = runAotChecked(kGenericHolder);
    expectClean(r, 0, "200\n");
}

// CONTROL 1: the LOCAL dictionary of records is already clean and stays clean.
BEZEL_TEST(ownership_owned_dict_field_record_value_box_9183,
           control_local_dictionary_of_records_is_already_clean) {
    const JitResult r = runAotChecked(kControlLocalRecord);
    expectClean(r, 0, "200\n");
}

// CONTROL 2: the OWNING dictionary's class values are disposed through the
// element policy row, and stay that way.
BEZEL_TEST(ownership_owned_dict_field_record_value_box_9183,
           control_owning_dictionary_field_is_unchanged) {
    const JitResult r = runAotChecked(kControlOwningDictionary);
    expectClean(r, 0, "200\n");
}
