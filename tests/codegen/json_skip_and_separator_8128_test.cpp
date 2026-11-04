// Regression tests for #8128 — the JSON unknown-field SKIP path leaks the
// SerializationException it raises when a NESTED value is malformed
// (stdlib §2.15, v0.355).
//
// THE SPEC SENTENCE.  stdlib §2.15 (docs/Bezel_Standard_Library.md:1073), the
// "Errors" paragraph:
//
//   "**Errors.** Malformed input, type mismatches (a `ReadInt` when the next
//   token is a string), and depth-limit violations raise
//   `SerializationException` (declared in `System.Serialization`). Trailing
//   data is the **whole-document** read forms' condition: the facades' reads
//   assert document exhaustion and raise it (above), while the agent path
//   deliberately leaves remaining input unread and never faults on it — the
//   cursor may rest mid-stream for a subsequent read, which is exactly how a
//   multi-value stream is read — and deserializer disposal never raises."
//
// and stdlib §2.15 (docs/Bezel_Standard_Library.md:953), the "JSON mapping"
// paragraph, which fixes what a malformed number is:
//
//   "A *JSON number* is JSON's own number token (RFC 8259 §6) … Any other
//   spelling of a number — `.5`, `5.`, `1.e3`, `+5`, `007`, `1_000`, `0x1p3` —
//   is malformed input (**Errors** below)."
//
// THE DEFECT.  `bz_jder_skip_value_impl`
// (src/runtime/bezel_runtime_serialization.cpp) skips a value by parsing it.
// When a NESTED value inside the skipped value is malformed, the nested parse
// raises — `bz_ser_throw` builds the exception object, stores it in the
// thread-local `bz_exc` slot and RETURNS (the generated code checks the slot
// after the call; it is not a C++ throw).  The skip path then carried on
// parsing and raised a SECOND exception ("Expected ',' or ']' in JSON array
// skip (§2.15)" / "Expected ',' or '}' in JSON object skip (§2.15)"), which
// OVERWROTE the slot with no regard for the exception already in it.  The
// first exception object — and its own heap-allocated Message string — was
// orphaned: one leaked exception per call (100 calls → 100 leaked exception
// objects), and the caller observed the second, less specific diagnostic
// instead of the genuine one.
//
// THE FIX.  Every nested failure in the skip path is PROPAGATED (the skip
// returns without raising again) instead of being carried on from, so exactly
// one exception is raised per call and it is the genuine one.
//
// THE ORACLE.  Two rows, on two oracles:
//   * the LEAK row runs the issue's own 100-iteration loop under
//     `runAotChecked` (the generated-code ASan/LSan leg): pre-fix the
//     sanitizer fires and the row fails; post-fix it is clean and the program
//     reports 42 (all 400 reads raised);
//   * the DIAGNOSTIC row (plain `runAot`) asserts the caught exception's
//     Message is the genuine one ("JSON number is malformed" / "Unexpected
//     JSON token in skip") and NOT the superseding one ("… in JSON array
//     skip" / "… in JSON object skip").  A coarse "did it raise SOME
//     SerializationException" assertion would pass on the pre-fix superseding
//     exception too, so this row reads the message text — the same shape as
//     serialization_case_discriminator_double_throw_2427_test.cpp's
//     message-correctness control.
//
// The CONTROLS are the shapes that raise exactly ONE exception on both arms
// (a malformed number directly in the skipped field) and the shape that
// raises none (a well-formed skipped field): the fix must not change them.
//
// APIs verified against Bezel_Standard_Library.md: JsonDeserializer /
// JsonOptions / SerializationException (§2.15).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// runAotChecked returns ran=false on ANY sanitizer fire (leak / UAF / crash);
// mapping that to a sentinel lets a single EXPECT_EQ pin both memory-
// cleanliness AND the program's own exit value in one assertion (mirrors
// serialization_case_discriminator_double_throw_2427_test.cpp's runChecked).
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImport = "import System.Serialization; ";

// The record the probes use: one KNOWN field (W) and one unknown field (U),
// so the unknown field takes the skip path.  `JsonOptions(false, 2, false,
// true, 64)` = Indented false, IndentWidth 2, AllowTrailingCommas false,
// IgnoreUnknownFields TRUE (the unknown field is skipped rather than
// rejected), MaxDepth 64 (stdlib §2.15's JsonOptions).
const char* kRecord =
    "record RS(W: int): ISerializable, IDeserializable; ";

// The inputs whose SKIPPED value contains a NESTED malformed value.
//
// The first three raise TWICE pre-fix (the nested failure, then the skip
// path's own "Expected ',' or ']'" / "Expected ',' or '}'"), so each leaks
// exactly one exception object:
//
//   A: the skipped value is an ARRAY whose first element is a malformed
//      number ("1.") — the array branch's separator check then fires.  This
//      is the issue's own repro row (r39-bisect-p06b/probes/loop.bzl's
//      S("{\"U\": [1., 2], \"W\": 1}")).
//   B: the skipped value is an OBJECT whose member value is a malformed
//      number ("01") — the object branch's separator check then fires.
//   C: the skipped value is an ARRAY whose first element is not a value at
//      all ("@") — the "Unexpected JSON token in skip" failure, then the
//      array branch's separator check.
//
// The CONTROL input raises exactly ONCE on both arms (the malformed number is
// the skipped field's own value, so no nested parse is involved and there is
// no second raise): the fix must leave its diagnostic untouched.  It is the
// issue's own repro's other S(...) row.
const char* kNumInArray = "{\"U\": [1., 2], \"W\": 1}";
const char* kNumInObject = "{\"U\": {\"x\": 01}, \"W\": 1}";
const char* kTokenInArray = "{\"U\": [@], \"W\": 1}";
const char* kNumDirect = "{\"U\": 01, \"W\": 1}";

// A Bezel string literal holding `text`.
std::string lit(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// The program the LEAK row runs: 100 iterations over the four inputs, each
// read expected to raise SerializationException — 400 reads.  The row asserts
// both "every call raised" and "nothing leaked": the sanitizer fire turns the
// row into -999, and the program reports 42 only when all 400 reads raised.
//
// (The count itself cannot be the exit status: a process exit status is 8
// bits, and 400 would be truncated to 144 — the compiler says so outright:
// "Main(): int return value 400 exceeds the 8-bit process exit-code range
// (0-255); the OS truncates this to 144 (§33.3)".)
//
// The four inputs are the issue's own two S(...) repro rows (the leaking
// "[1., 2]" one and the single-raise "01" control) plus the two further
// nested-failure shapes of the skip path (the object branch and the
// non-value token).
std::string loopProgram() {
    return std::string("module M; ") + kImport + kRecord +
           "class C public static method Main(): int "
           "  var raises: int := 0; "
           "  var i: int := 0; "
           "  while i < 100 do "
           "    raises := raises + One(" + lit(kNumInArray) + "); "
           "    raises := raises + One(" + lit(kNumInObject) + "); "
           "    raises := raises + One(" + lit(kTokenInArray) + "); "
           "    raises := raises + One(" + lit(kNumDirect) + "); "
           "    i := i + 1; "
           "  end; "
           "  return (if raises = 400 then 42 else 1); "
           "end Main; "
           "private static method One(j: string): int "
           "  using var d := new JsonDeserializer(j, "
           "new JsonOptions(false, 2, false, true, 64)); "
           "  try "
           "    var v := RS.Deserialize(d); "
           "    return 0; "
           "  catch SerializationException => return 1; end; "
           "end One; "
           "end C;";
}

// The program the DIAGNOSTIC row runs: one read of `json`, returning 42 when
// the caught exception's Message is the genuine one and 2 when it is the
// superseding one (and 3 when nothing was raised at all).
//
// `genuine` is the substring the genuine diagnostic carries; `superseding` is
// the substring the pre-fix superseding diagnostic carries.  Exactly one of
// them must be present.
std::string kindProgram(const std::string& json, const std::string& genuine,
                         const std::string& superseding) {
    return std::string("module M; ") + kImport + kRecord +
           "class C public static method Main(): int "
           "  using var d := new JsonDeserializer(" + lit(json) + ", "
           "new JsonOptions(false, 2, false, true, 64)); "
           "  try "
           "    var v := RS.Deserialize(d); "
           "    return 3; "
           "  catch var e: SerializationException => "
           "    return (if e.Message.Contains(" + lit(genuine) + ") and "
           "               not e.Message.Contains(" + lit(superseding) + ") "
           "            then 42 else 2); "
           "  end; "
           "end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE LEAK — the issue's own 100-iteration loop, under the ASan/LSan leg.
//
// Pre-fix: one exception object (and its message string) leaks per call, so
// the sanitizer fires and runChecked returns -999.
// Post-fix: clean, and all 400 reads raised → 42.
// ===========================================================================
BEZEL_TEST(json_skip_and_separator_8128,
           nested_unknown_field_skip_is_leak_free_spec_2_15) {
    BEZEL_EXPECT_EQ(runChecked(loopProgram()), 42);
}

// ===========================================================================
// THE DIAGNOSTIC — the raised exception must be the GENUINE one.
//
// Pre-fix: the skip path's second raise superseded the nested failure's
// exception, so the caught Message is "Expected ',' or ']' in JSON array skip
// (§2.15)" / "Expected ',' or '}' in JSON object skip (§2.15)" → 2.
// Post-fix: the genuine nested diagnostic survives → 42.
// ===========================================================================
BEZEL_TEST(json_skip_and_separator_8128,
           nested_unknown_field_skip_raises_the_genuine_diagnostic_spec_2_15) {
    // A: the nested malformed number inside a skipped array.
    BEZEL_EXPECT_EQ(
        run(kindProgram(kNumInArray, "JSON number is malformed",
                        "in JSON array skip")),
        42);
    // B: the nested malformed number inside a skipped object.
    BEZEL_EXPECT_EQ(
        run(kindProgram(kNumInObject, "JSON number is malformed",
                        "in JSON object skip")),
        42);
    // C: the nested non-value token inside a skipped array.
    BEZEL_EXPECT_EQ(
        run(kindProgram(kTokenInArray, "Unexpected JSON token in skip",
                        "in JSON array skip")),
        42);
}

// ===========================================================================
// THE CONTROLS — the shapes that raise exactly ONE exception (or none) must
// keep raising it, with the same diagnostic, on both arms.
// ===========================================================================
BEZEL_TEST(json_skip_and_separator_8128,
           single_raise_in_the_skipped_field_keeps_its_diagnostic_spec_2_15) {
    // The malformed number IS the skipped field's own value: one raise, no
    // nested parse, so no second raise and no leak pre-fix either.
    BEZEL_EXPECT_EQ(
        run(kindProgram(kNumDirect, "JSON number is malformed",
                        "in JSON array skip")),
        42);
}

// A well-formed skipped field must still be skipped silently (no raise at
// all): the fix must not turn the happy path into a failure.
BEZEL_TEST(json_skip_and_separator_8128,
           well_formed_unknown_field_is_still_skipped_spec_2_15) {
    BEZEL_EXPECT_EQ(
        run(std::string("module M; ") + kImport + kRecord +
            "class C public static method Main(): int "
            "  using var d := new JsonDeserializer(\"{\\\"U\\\": 1, \\\"W\\\": 1}\", "
            "new JsonOptions(false, 2, false, true, 64)); "
            "  try "
            "    var v := RS.Deserialize(d); "
            "    return (if v.W = 1 then 42 else 2); "
            "  catch SerializationException => return 3; end; "
            "end Main; end C;"),
        42);
}
