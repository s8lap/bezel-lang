// Codegen / disposal — a `Dictionary<K, V>` value box over a value type that
// carries a `string` must hold its OWN reference on that string: issues #9197
// (the GENERIC `Dictionary<string, T>` spelling) and #9254 (the CHOICE value,
// generic or not).
//
// The value is heap-boxed (stdlib §4.8's value encoding) and the box is the
// collection's own storage. §28.11's element slot policy gives the BORROWING
// family's slot for a value-type element the COPY policy — the store row's
// "copies the value in" — and the copy the collection holds is one of the
// holders of the value's runtime-managed references (§28.9: a `string` is
// runtime-managed; the runtime reclaims it once the last holder is gone). A
// holder that gives a reference back must first have taken one: the box's
// RELEASE half (`Emitter::dictValueBoxFreer` → `boxedRecordValueFreer` /
// `boxedChoiceValueFreer`) and its TAKE half (`Emitter::
// boxValueNeedsStringRetain`, fix #3974) are a PAIR.
//
// #9197. Inside a monomorphized generic body the dictionary's V is the
// placeholder `T` (no `declaration`), so NEITHER half fired. The first pass
// (746c26e64) resolves `T` through the active substitution for BOTH halves
// (`Emitter::dictValueTyEff`, §14.1), so a generic V reaches the same pair its
// non-generic twin reaches.
//
// #9254. For a CHOICE V the take half returned false unconditionally, so the
// box released a reference it never took whenever the source was a BORROW: a
// named local, a field, a getter's result, or a case construction whose
// string payload argument is itself a borrow. Non-generic: a use-after-free on
// r30-c1. Generic: after the first pass resolved `T` for the release half, the
// same fault reached every generic `Dictionary<K, T>` at a choice `T` (review
// r43-rev-m104-9197: a heap-use-after-free for `h.Put(s)` followed by a read
// of `s`, and a "refcount over-release" abort for a value stored twice). The
// take half's choice arm is now the record arm's twin: no retain only for a
// provably FRESH construction (every string payload argument a +1 producer or
// a literal), a synthesized `Deserialize`, or a call to a method whose whole
// body is such a construction; every other source retains.
//
// THE ORACLE. Every RED row runs under the lenient oracle (generated-code
// ASan, no LeakSanitizer) and pins the run's stdout. The fault each pins is a
// use-after-free or an over-release abort, which the lenient oracle catches;
// LeakSanitizer is left out because these programs ALSO leak through
// separate, pre-existing source-side defects that leak the same bytes with and
// without the box (a by-value value-type argument is never released by its
// caller — #5865 / #6153 for a record; a value-like choice local that is
// copied into a box is never released at its scope end). Those leaks are not
// the box's, and a row that asserted their absence would pin the wrong
// defect.
//
// The two controls run under the STRICT oracle and are green on the base by
// construction; each is red under a plausible wrong fix:
//  * a FRESH case construction stored straight into the box must stay
//    leak-free — a take half that retained EVERY choice source would leak
//    the construction's own +1 here;
//  * the issue's own literal repro (a generic RECORD value) must run without
//    a use-after-free or an over-release (lenient: its residual leak is the
//    #5865 / #6153 source-side one).
//
// Spec: §28.9 (runtime-managed types), §28.11 (the element slot policy — the
// copy-slot store row and the Dispose row), §14.1 (generic instantiation),
// §9.7 / §35.4 (a choice's payload), stdlib §4.8 (the dictionary's value
// encoding). Spec revision v0.359. Every program loops 200 times.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// Ran to completion with the expected stdout, and no generated-code ASan
// report and no runtime over-release abort.
void expectNoUafLenient(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

void expectCleanStrict(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The review's repro (r43-rev-m104-9197 uaf_generic_choice_local.bzl), looped:
// a NAMED choice local is stored into a generic dictionary inside `Put`, the
// dictionary is disposed at `Put`'s end, and the local is read afterwards.
const char* kGenericChoiceNamedLocal =
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
    "    var total := 0; "
    "    while i < 200 do "
    "      var s := Shape.Named(\"p\" + (i + 1000).ToString()); "
    "      using var h := new Holder<Shape>(); "
    "      total := total + h.Put(s); "
    "      total := total + (case s of Named(n) => n.Length, Circle(r) => 0); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "  end Main; "
    "end Program; ";

// The review's overwrite and Clear-then-store shapes, in one generic body:
// the same choice value stored twice, then cleared and stored again.
const char* kGenericChoiceOverwriteClear =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class Holder<T> "
    "  public method Put(v: T): int "
    "    var d := new Dictionary<string, T>(); "
    "    d[\"a\"] := v; "
    "    d[\"a\"] := v; "
    "    d.Clear(); "
    "    d[\"b\"] := v; "
    "    return d.Count; "
    "  end Put; "
    "end Holder; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var total := 0; "
    "    while i < 200 do "
    "      var s := Shape.Named(\"p\" + (i + 1000).ToString()); "
    "      using var h := new Holder<Shape>(); "
    "      total := total + h.Put(s); "
    "      total := total + (case s of Named(n) => n.Length, Circle(r) => 0); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "  end Main; "
    "end Program; ";

// #9254, non-generic: a named choice local stored, removed, then read.
const char* kChoiceLocalRemoveThenRead =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var total := 0; "
    "    while i < 200 do "
    "      var s := Shape.Named(\"p\" + (i + 1000).ToString()); "
    "      var d := new Dictionary<string, Shape>(); "
    "      d[\"a\"] := s; "
    "      var rm := d.Remove(\"a\"); "
    "      total := total + (case s of Named(n) => n.Length, Circle(r) => 0); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "  end Main; "
    "end Program; ";

// #9254, non-generic: the same named choice local stored twice (the overwrite
// gives the first box back), then read through the dictionary and directly.
const char* kChoiceLocalStoredTwice =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var total := 0; "
    "    while i < 200 do "
    "      var s := Shape.Named(\"p\" + (i + 1000).ToString()); "
    "      var d := new Dictionary<string, Shape>(); "
    "      d[\"a\"] := s; "
    "      d[\"a\"] := s; "
    "      total := total + (case d[\"a\"] of Named(n) => n.Length, "
    "Circle(r) => 0); "
    "      total := total + (case s of Named(n) => n.Length, Circle(r) => 0); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "  end Main; "
    "end Program; ";

// #9254, non-generic: a getter's result (a copy of a FIELD) stored, the
// dictionary cleared, then the field read.
const char* kChoiceGetterResultClearThenReadField =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class FG "
    "  private _s: Shape := Shape.Circle(1); "
    "  public method SetS(i: int) => _s := Shape.Named(\"f\" + (i + 1000)"
    ".ToString()); "
    "  public method GetS(): Shape => _s; "
    "  public method ReadS(): int => (case _s of Named(n) => n.Length, "
    "Circle(r) => 0); "
    "end FG; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var total := 0; "
    "    while i < 200 do "
    "      using var h := new FG(); "
    "      h.SetS(i); "
    "      var d := new Dictionary<string, Shape>(); "
    "      d[\"a\"] := h.GetS(); "
    "      total := total + (case d[\"a\"] of Named(n) => n.Length, "
    "Circle(r) => 0); "
    "      d.Clear(); "
    "      total := total + h.ReadS(); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "  end Main; "
    "end Program; ";

// #9254, non-generic: a case construction whose string payload argument is a
// BORROW (a parameter) — the construction stores the pointer raw, so the box
// must take its own reference before `Clear` gives it back.
const char* kChoiceCaseOfBorrowedStringClearThenRead =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class SP "
    "  public static method Go(str: string): int "
    "    var d := new Dictionary<string, Shape>(); "
    "    d[\"a\"] := Shape.Named(str); "
    "    d.Clear(); "
    "    return str.Length; "
    "  end Go; "
    "end SP; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var total := 0; "
    "    while i < 200 do "
    "      var str := \"p\" + (i + 1000).ToString(); "
    "      total := total + SP.Go(str) + str.Length; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "  end Main; "
    "end Program; ";

// CONTROL (strict): a FRESH case construction stored straight into the box,
// by the indexer and by `Add`. The construction's +1 is the box's; no retain.
const char* kChoiceFreshConstructionStored =
    "module M; "
    "public choice Shape Circle(r: int), Named(n: string); end Shape; "
    "public class Program "
    "  public static method Main() "
    "    var i := 0; "
    "    var total := 0; "
    "    while i < 200 do "
    "      var d := new Dictionary<string, Shape>(); "
    "      d[\"a\"] := Shape.Named(\"p\" + (i + 1000).ToString()); "
    "      d.Add(\"b\", Shape.Named(\"q\" + (i + 1000).ToString())); "
    "      total := total + (case d[\"a\"] of Named(n) => n.Length, "
    "Circle(r) => 0); "
    "      total := total + (case d[\"b\"] of Named(n) => n.Length, "
    "Circle(r) => 0); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "  end Main; "
    "end Program; ";

// CONTROL (lenient): the issue's own literal repro — a generic RECORD value.
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

BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           generic_choice_named_local_source_keeps_its_string_spec_28_9) {
    expectNoUafLenient(runAotCheckedLenient(kGenericChoiceNamedLocal),
                       "1200\n");
}

BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           generic_choice_stored_twice_and_after_clear_spec_28_11) {
    expectNoUafLenient(runAotCheckedLenient(kGenericChoiceOverwriteClear),
                       "1200\n");
}

BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           choice_named_local_removed_then_read_9254_spec_28_9) {
    expectNoUafLenient(runAotCheckedLenient(kChoiceLocalRemoveThenRead),
                       "1000\n");
}

BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           choice_named_local_stored_twice_9254_spec_28_11) {
    expectNoUafLenient(runAotCheckedLenient(kChoiceLocalStoredTwice),
                       "2000\n");
}

BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           choice_getter_result_cleared_then_field_read_9254_spec_28_9) {
    expectNoUafLenient(
        runAotCheckedLenient(kChoiceGetterResultClearThenReadField), "2000\n");
}

BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           choice_case_of_borrowed_string_cleared_then_read_9254_spec_35_4) {
    expectNoUafLenient(
        runAotCheckedLenient(kChoiceCaseOfBorrowedStringClearThenRead),
        "2000\n");
}

BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           control_fresh_choice_construction_stored_is_leak_free) {
    expectCleanStrict(runAotChecked(kChoiceFreshConstructionStored), "2000\n");
}

BEZEL_TEST(ownership_generic_dict_value_box_string_release_9197,
           control_the_issue_repro_runs_without_a_use_after_free) {
    expectNoUafLenient(runAotCheckedLenient(kIssueRepro), "200\n");
}
