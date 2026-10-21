// #6789 -- spec §35.3, v0.340.
//
// §35.3 (v0.340): "Construction is qualified, resolving exactly as enum
// constants do: `FetchOutcome.NotModified` for a payload-free case (a value
// expression, not a call), `FetchOutcome.Failed(504, "upstream timeout")` for
// a payload case." Appendix C.7 states the matching-side twin: "a
// payload-carrying case may not be matched bare".
//
// MEASURED at base d952ddd0~2: `var f := Outcome.Fetched;` compiled under
// warning #2434 ("initializer expression failed to lower ... degraded to a
// zero-initialized value") and the program ran on tag 0 -- `case f of
// NotModified => ...` took the FIRST arm for `var f := Outcome.Failed;`
// (stdout `nm`, exit 0). §35.3 declares no case-constructor function value, so
// the bare qualified payload case denotes no value. The reject rows fail
// without the fix; the accept controls (payload-free bare, call form, the
// declared-result bare payload-free case, a generic choice) pass on both sides.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char kDecls[] =
    "module M; "
    "choice Outcome NotModified, Fetched(Status: int), Failed(Msg: string); "
    "end Outcome; "
    "choice Pair None, Two(A: int, B: int); end Pair; "
    "choice Opt<T> Some(V: T), None; end Opt; "
    "class Util "
    "  public static method Take(o: Outcome): int return 1; end Take; "
    "  public static method GiveNm(): Outcome return Outcome.NotModified; end GiveNm; "
    "end Util; ";

std::string prog(const std::string& body, const std::string& extra = "") {
    return std::string(kDecls) + extra +
           "public class Program "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void expectOut(const std::string& body, const char* want) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

void expectRejected(const std::string& body, const char* caseName) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, (std::string("payload case `") + caseName +
            "` cannot be used as a value without its payload").c_str()));
}

}  // namespace

// --- reject ------------------------------------------------------------------

// B17: the issue's first repro.
BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_bare_payload_case_in_var_initializer_rejected) {
    expectRejected("var f := Outcome.Fetched; Console.WriteLine(\"v\"); ",
                   "Fetched");
}

// b13_nm: the zero tag used to select the FIRST arm.
BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_bare_payload_case_then_matched_rejected) {
    expectRejected(
        "var f := Outcome.Failed; "
        "var s := (case f of NotModified => \"nm\", _ => \"other\"); "
        "Console.WriteLine(s); ",
        "Failed");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_bare_payload_case_typed_local_rejected) {
    expectRejected("var f: Outcome := Outcome.Fetched; "
                   "Console.WriteLine(\"v\"); ",
                   "Fetched");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_bare_payload_case_assignment_rejected) {
    expectRejected("var f := Outcome.NotModified; f := Outcome.Failed; "
                   "Console.WriteLine(\"v\"); ",
                   "Failed");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_bare_payload_case_function_argument_rejected) {
    expectRejected("Console.WriteLine(Util.Take(Outcome.Fetched)); ",
                   "Fetched");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_bare_payload_case_return_value_rejected) {
    // The error is in the body of `Give`, a bare payload case as the value of
    // `return`.
    auto r = runAot(prog("Console.WriteLine(Util.Take(Outcome.NotModified)); ",
                         "public class Giver "
                         "  public static method Give(): Outcome "
                         "    return Outcome.Fetched; "
                         "  end Give; "
                         "end Giver; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "payload case `Fetched` cannot be used as a value"));
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_bare_multi_slot_payload_case_rejected) {
    expectRejected("var p := Pair.Two; Console.WriteLine(\"v\"); ", "Two");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_bare_generic_payload_case_rejected) {
    expectRejected("var o := Opt<int>.Some; Console.WriteLine(\"v\"); ",
                   "Some");
}

// --- accept controls ---------------------------------------------------------

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_payload_free_case_bare_value_accepted) {
    expectOut("var f := Outcome.NotModified; "
              "var s := (case f of NotModified => \"nm\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "nm\n");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_payload_case_call_form_accepted) {
    expectOut("var f := Outcome.Failed(\"xy\"); "
              "var s := (case f of Failed(var m) => $\"m={m.Length}\", "
              "  _ => \"other\"); Console.WriteLine(s); ",
              "m=2\n");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_payload_case_call_form_as_argument_accepted) {
    expectOut("Console.WriteLine(Util.Take(Outcome.Fetched(5))); ", "1\n");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_returned_qualified_payload_free_case_accepted) {
    expectOut("var f := Util.GiveNm(); "
              "var s := (case f of NotModified => \"nm\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "nm\n");
}

BEZEL_TEST(sema_choice_bare_payload_case_value_6789,
           spec_35_3_generic_payload_free_and_call_forms_accepted) {
    expectOut("var a := Opt<int>.None; var b := Opt<int>.Some(3); "
              "var s := (case b of Some(var v) => $\"{v}\", _ => \"n\"); "
              "var t := (case a of None => \"none\", _ => \"x\"); "
              "Console.WriteLine(s); Console.WriteLine(t); ",
              "3\nnone\n");
}
