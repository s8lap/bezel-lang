// Regression test for issue #9048 — an array of VALUE-LIKE choices serializes
// but does not deserialize past its first element (spec §9.8, v0.359).
//
// THE DEFECT.  §9.8's admissible-component list reads (v0.359):
//
//   "A component type is **admissible as a serialized component** when it is:
//    … a value-like serialization-eligible choice that itself declares
//    `ISerializable`/`IDeserializable` …; … or a standard sequence collection
//    (`Array<T>`, `OwningArray<T>`, `LinkedList<T>`, etc., and inline arrays
//    `[N]T`) whose element type is admissible as a serialized component
//    (encoded with `BeginArray`/`EndArray`)."
//
// So `Array<Shape>` with a value-like serializable `Shape` IS an admissible
// component, and the wire form is a JSON array of `$case`-discriminated
// objects.  The SERIALIZE half writes that array correctly.  The DESERIALIZE
// half stopped after the FIRST element.
//
// THE ROOT CAUSE.  The JSON reader's element/member separator (the ',') is
// taken by the reader of the VALUE that follows it
// (`bz_jder_take_value_separator`): `bz_binder_begin_object` (a record or
// class component), `bz_binder_begin_array` (an array component) and every
// primitive reader take it.  The CHOICE reader
// (`bz_binder_read_case_discriminator`, §9.8's "$case" dispatch) did NOT: it
// went straight to the '{' check.  So after element 0 the cursor still sat on
// the ',' and the next element was rejected with
//
//   Unhandled exception: System.SerializationException: Choice deserialize:
//   expected '{' (§9.8)
//
// The SAME shape read as a record or an enum (whose readers DO take the
// separator) deserialized every element — the asymmetry the fix removes.
//
// MEASURED on the base (r30-c1 @ 31718ac3b), `bezelc --sanitize=address` on
// the issue's literal repro: the program compiles cleanly and then raises
//   "System.SerializationException: Choice deserialize: expected '{' (§9.8)"
//   at the SECOND element (exit 70); with the fix it prints "2" and exits 0.
//
// THE SPEC also decides the READ-SIDE framing (stdlib §2.15): "On the read
// side `JsonDeserializer.BeginArray()` returns `-1`, so consumers use
// `HasMoreElements()`."  The hand-driven rows below use exactly that idiom.
//
// ORACLE.  `runAot` for the behavioural rows (the issue's own program's shape,
// plus the synthesized `owned Array<Shape>` class field and the `[2]Shape`
// inline-array record field), `runAotChecked` for the 200-iteration row that
// also asserts LeakSanitizer counted nothing — the `$case` discriminator is a
// heap string the reader allocates per element, and the row proves it is
// released EXACTLY once per element.
//
// ROWS.  Five rows are RED on the base (they raise at the second element);
// two are the suite's GREEN boundary controls (a one-element array — the
// issue's stated-correct twin — and the empty array).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes.  LSan
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

// The program ran to completion: it compiled, it exited with the program's own
// exit code and printed exactly the expected output.  `runAot` reports a
// compile rejection as `ran == false`, so the `ran` assertion is the one that
// says "the program really ran".
void expectRan(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The checked AOT path: the program's own output AND that neither
// AddressSanitizer nor LeakSanitizer fired.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectRan(r, exitCode, out);
    assertNoLeak(r);
}

// Replace every occurrence of `needle` in `text` with `repl`.
std::string replaceAll(std::string text, const std::string& needle,
                        const std::string& repl) {
    const std::size_t n = needle.size();
    if (n == 0) return text;
    std::size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        text.replace(at, n, repl);
        at += repl.size();
    }
    return text;
}

// The hand-driven reader of stdlib §2.15's JSON array idiom: BeginArray,
// then HasMoreElements until the array ends, then EndArray.  Each element is
// read with the choice's own synthesized Deserialize (§9.8).
//
// `jsonLiteral` is the Bezel string literal holding the JSON array (with the
// Bezel escapes already applied); `sumLiteral` is the Bezel literal the
// payload sum is compared against.  The program prints the element COUNT and
// a payload checksum (1 when the payloads summed as expected, 0 otherwise),
// so a row cannot pass by counting elements alone.
std::string handDriven(const std::string& jsonLiteral, const char* sumLiteral) {
    const std::string tmpl = R"BZ(
module P;
import System.Serialization;
public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double),
  None;
end Shape;
public class Program
  public static method Main(): int
    using var d := new JsonDeserializer(@JSONLITERAL@);
    _ := d.BeginArray();
    var n := 0;
    var sum := 0.0;
    while d.HasMoreElements() do
      var sh := Shape.Deserialize(d);
      sum := sum + (case sh of Circle(var r) => r, Rect(var w, var h) => w + h, None => 0.0);
      n++;
    end;
    d.EndArray();
    Console.WriteLine(n);
    Console.WriteLine((if sum = @SUMLITERAL@ then 1 else 0));
    return 0;
  end Main;
end Program;
)BZ";
    std::string out = replaceAll(tmpl, "@JSONLITERAL@", jsonLiteral);
    return replaceAll(out, "@SUMLITERAL@", sumLiteral);
}

// ===========================================================================
// §9.8: the hand-driven array of value-like choices.
// ===========================================================================

// ROW 1 — the issue's shape, with THREE elements (the issue's repro has two).
// RED on the base: raises "Choice deserialize: expected '{' (§9.8)" at the
// SECOND element (the count printed is 1, never 3).
BEZEL_TEST(serialization_choice_array_deserialize_9048,
           three_element_array_reads_every_element_spec_9_8) {
    expectRan(runAot(handDriven(
                   R"j("[{\"$case\":\"Circle\",\"r\":1},{\"$case\":\"Rect\",\"w\":2,\"h\":3},{\"$case\":\"Circle\",\"r\":1}]")j",
                   "7.0")),
               0, "3\n1\n");
}

// ROW 2 — MIXED cases: a payload-free case and a case with a payload, in one
// array.  The payload-free case writes the object with ONLY the discriminator
// (§9.8), so its object is shorter than a payload case's; the row proves the
// reader resumes at the next element in BOTH shapes.
//
// Elements: None, Circle(1.0), None, Rect(2.0, 3.0) — payload sum 6.0.
//
// RED on the base: raises at the SECOND element.
BEZEL_TEST(serialization_choice_array_deserialize_9048,
           mixed_payload_free_and_payload_cases_read_every_element_spec_9_8) {
    expectRan(runAot(handDriven(
                   R"j("[{\"$case\":\"None\"},{\"$case\":\"Circle\",\"r\":1},{\"$case\":\"None\"},{\"$case\":\"Rect\",\"w\":2,\"h\":3}]")j",
                   "6.0")),
               0, "4\n1\n");
}

// ROW 3 — the SYNTHESIZED form: a class that owns an `Array<Shape>` field
// (§9.8's sequence-collection row: "encoded with `BeginArray`/`EndArray`").
// The synthesized Deserialize drives the element loop with HasMoreElements
// (stdlib §2.15) and reads each element with the choice's own Deserialize.
//
// RED on the base: the synthesized reader raises at the SECOND element.
BEZEL_TEST(serialization_choice_array_deserialize_9048,
           owned_array_choice_field_reads_every_element_spec_9_8) {
    const std::string src = R"BZ(
module P;
import System.Serialization;
public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double),
  None;
end Shape;
public class Bag(owned items: Array<Shape>): ISerializable, IDeserializable
  public owned Items: Array<Shape> := items;
end Bag;
public class Program
  public static method Main(): int
    using var b := new Bag(new Array<Shape>());
    b.Items.Add(Shape.Circle(1.0));
    b.Items.Add(Shape.Rect(2.0, 3.0));
    b.Items.Add(Shape.None);
    using var s := new JsonSerializer();
    b.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    using var b2 := Bag.Deserialize(d);
    Console.WriteLine(b2.Items.Count);
    var sum := 0.0;
    for var i in 0..<3 do
      sum := sum + (case b2.Items[i] of Circle(var r) => r, Rect(var w, var h) => w + h, None => 0.0);
    end;
    Console.WriteLine((if sum = 6.0 then 1 else 0));
    return 0;
  end Main;
end Program;
)BZ";
    expectRan(runAot(src), 0, "3\n1\n");
}

// ROW 4 — the inline-array record component: `record Pair(A: [2]Shape)`.  §9.8
// admits an inline array `[N]T` whose element type is admissible, and an
// inline array is a VALUE type, so the record clause (heap collections are
// class-only) does not bar it.
//
// RED on the base: the second element of the inline array raises.
BEZEL_TEST(serialization_choice_array_deserialize_9048,
           inline_array_choice_record_field_reads_every_element_spec_9_8) {
    const std::string src = R"BZ(
module P;
import System.Serialization;
public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double),
  None;
end Shape;
public record Pair(A: [2]Shape): ISerializable, IDeserializable;
public class Program
  public static method Main(): int
    const p := new Pair([Shape.Circle(1.0), Shape.Rect(2.0, 3.0)]);
    using var s := new JsonSerializer();
    p.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    const p2 := Pair.Deserialize(d);
    var sum := 0.0;
    for var i in 0..<2 do
      sum := sum + (case p2.A[i] of Circle(var r) => r, Rect(var w, var h) => w + h, None => 0.0);
    end;
    Console.WriteLine((if sum = 6.0 then 1 else 0));
    return 0;
  end Main;
end Program;
)BZ";
    expectRan(runAot(src), 0, "1\n");
}

// ROW 5 — the 200-iteration loop, under the checked AOT path: the `$case`
// discriminator is a heap string the reader allocates per element, and the row
// proves it is released EXACTLY once per element (200 iterations × 3 elements
// = 600 discriminator strings).
//
// RED on the base: the very first iteration raises at the second element, so
// the row's expected total (600) is never printed.
BEZEL_TEST(serialization_choice_array_deserialize_9048,
           repeated_reads_release_every_discriminator_spec_9_8) {
    const std::string src = R"BZ(
module P;
import System.Serialization;
public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double),
  None;
end Shape;
public class Program
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      using var d := new JsonDeserializer("[{\"$case\":\"Circle\",\"r\":1},{\"$case\":\"Rect\",\"w\":2,\"h\":3},{\"$case\":\"Circle\",\"r\":1}]");
      _ := d.BeginArray();
      var n := 0;
      var sum := 0.0;
      while d.HasMoreElements() do
        var sh := Shape.Deserialize(d);
        sum := sum + (case sh of Circle(var r) => r, Rect(var w, var h) => w + h, None => 0.0);
        n++;
      end;
      d.EndArray();
      if sum <> 7.0 then return 91; end;
      total := total + n;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ";
    expectClean(runAotChecked(src), 0, "600\n");
}

// ===========================================================================
// §9.8: the boundary controls (GREEN on the base).
// ===========================================================================

// ROW 6 — the ONE-ELEMENT array: the issue's stated-correct twin ("A single
// choice … round-trips").  This row is GREEN on the base — the separator
// problem only bites from the SECOND element on — so it is the suite's
// boundary control: the fix must not change the single-element read.
//
// The wrong fix it catches: a fix that consumed the separator unconditionally
// (taking a ',' that is not there), which would make the FIRST element fail
// with "Unexpected ',' in JSON (§2.15)".
BEZEL_TEST(serialization_choice_array_deserialize_9048,
           one_element_array_still_reads_spec_9_8) {
    expectRan(runAot(handDriven(R"j("[{\"$case\":\"Circle\",\"r\":1}]")j", "1.0")),
               0, "1\n1\n");
}

// ROW 7 — the EMPTY array: the array's own framing is read with no element
// read at all, so the separator is never taken.  GREEN on the base; it pins
// that the fix does not make the empty array raise.
//
// The wrong fix it catches: a fix that took a separator BEFORE the '[' (or
// that consumed the ']'), which would make the empty array raise.
BEZEL_TEST(serialization_choice_array_deserialize_9048,
           empty_array_still_reads_spec_9_8) {
    expectRan(runAot(handDriven("\"[]\"", "0.0")), 0, "0\n1\n");
}

} // namespace
