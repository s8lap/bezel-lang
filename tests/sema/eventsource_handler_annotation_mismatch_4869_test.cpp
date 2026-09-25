// fix #4869 (§17.2 / §27.1.1 / §5.1, v0.307; stdlib §6.5): the event-
// subscription forms `lhs += h` / `lhs -= h` on an `EventSource<T>` are, per
// §17.2, "a fixed, language-defined desugaring: `lhs += h` rewrites to
// `lhs.Add(h)` and `lhs -= h` to `lhs.Remove(h)`", and stdlib §6.5 declares
//   public method Add(scoped handler: (T) -> ());
//   public method Remove(handler: (T) -> ());
// So a handler lambda whose parameter is EXPLICITLY annotated with a type
// that cannot accept the payload `T` is a compile error at the position:
// §27.1.1 makes a primitive slot invariant, and no §5.1 implicit conversion
// runs `int -> string` either.
//
// Pre-fix the `+=` / `-=` arm threaded the payload into an UNTYPED lambda
// parameter (§27.4) but checked nothing — the lambda's explicit annotation
// wins over the seeded payload, and the §6.6 compound-assignment check is
// skipped for the subscription forms — so the issue's repro compiled, and at
// runtime the handler read the `int` payload as a string pointer (silent
// wrong code: it printed an empty line for `Fire(0)`).
//
// The ACCEPT rows RUN (runAot) and assert the printed payload, so the new
// check is proven not to over-reject the seeded / correctly-annotated /
// method-group shapes.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — AOT + native exec oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The unique substring of the #4869 diagnostic.
constexpr const char* kNeedle =
    "does not accept the event payload type `int`";

// Finds the FIRST diagnostic whose message contains `needle`, or nullptr.
const bezel::Diagnostic* findDiag4869(const bezel::test::JitResult& r,
                                      const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return &d;
    return nullptr;
}

std::string beeperProgram(const std::string& subscribeStmt,
                          const std::string& payloadTy,
                          const std::string& fireArg,
                          const std::string& extraMembers = "") {
    return "module M; import System; "
           "class Beeper "
           "  private owned _beep: EventSource<" + payloadTy + ">; "
           "  public constructor() "
           "    _beep := new EventSource<" + payloadTy + ">(); "
           "    " + subscribeStmt + " "
           "  end constructor; " + extraMembers +
           "  public method Fire(n: " + payloadTy + ") _beep.Raise(n); "
           "  end Fire; "
           "end Beeper; "
           "class C "
           "  public static method Main(): int "
           "    using var b := new Beeper(); "
           "    b.Fire(" + fireArg + "); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — the issue's own repro: `EventSource<int> += lambda(n: string)`.
// ---------------------------------------------------------------------------
BEZEL_TEST(eventsource_handler_annotation_mismatch_4869,
           plus_eq_string_annotation_on_int_source_rejected_spec_17_2) {
    auto r = runAot(beeperProgram(
        "_beep += lambda(n: string) => Console.WriteLine(n);", "int", "0"));
    const auto* d = findDiag4869(r, kNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(d->message.find("annotated `string`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(d->message.find("`Add(handler)`") != std::string::npos);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The `-=` form desugars to `Remove(handler: (T) -> ())` — same position,
// same check.
BEZEL_TEST(eventsource_handler_annotation_mismatch_4869,
           minus_eq_string_annotation_on_int_source_rejected_spec_17_2) {
    auto r = runAot(beeperProgram(
        "_beep -= lambda(n: string) => Console.WriteLine(n);", "int", "0"));
    const auto* d = findDiag4869(r, kNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(d->message.find("`Remove(handler)`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// ACCEPT — run and print the payload.
// ---------------------------------------------------------------------------

// Unannotated: the parameter takes the payload type (§27.4).
BEZEL_TEST(eventsource_handler_annotation_mismatch_4869,
           plus_eq_unannotated_lambda_runs_spec_27_4) {
    auto r = runAot(beeperProgram(
        "_beep += lambda(n) => Console.WriteLine(n + 1);", "int", "41"));
    BEZEL_EXPECT_TRUE(findDiag4869(r, kNeedle) == nullptr);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// Correctly annotated `lambda(n: int)` on `EventSource<int>`.
BEZEL_TEST(eventsource_handler_annotation_mismatch_4869,
           plus_eq_int_annotation_on_int_source_runs_spec_27_1_1) {
    auto r = runAot(beeperProgram(
        "_beep += lambda(n: int) => Console.WriteLine(n * 2);", "int", "21"));
    BEZEL_EXPECT_TRUE(findDiag4869(r, kNeedle) == nullptr);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// Method-group handler `_beep += OnBeep;` with `OnBeep(n: int)`.
BEZEL_TEST(eventsource_handler_annotation_mismatch_4869,
           plus_eq_method_group_runs_spec_17_2) {
    auto r = runAot(beeperProgram(
        "_beep += OnBeep;", "int", "7",
        "  private method OnBeep(n: int) Console.WriteLine(n + 100); "
        "  end OnBeep; "));
    BEZEL_EXPECT_TRUE(findDiag4869(r, kNeedle) == nullptr);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("107\n"));
}

// `EventSource<string>` with a `string`-annotated lambda — the annotation
// equals the payload type, so the position admits it.
BEZEL_TEST(eventsource_handler_annotation_mismatch_4869,
           plus_eq_string_annotation_on_string_source_runs_spec_27_1_1) {
    auto r = runAot(beeperProgram(
        "_beep += lambda(s: string) => Console.WriteLine(s + \"!\");",
        "string", "\"hi\""));
    BEZEL_EXPECT_TRUE(findDiag4869(r, "does not accept the event payload") ==
                      nullptr);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("hi!\n"));
}

// §27.1.1: "Value-typed, record-typed, and primitive slots are invariant. No
// numeric widening, boxing, or other representation-changing conversion
// applies" — and the only site-typed bridge a lambda literal gets is its
// return slot (§27.3). A `double`-annotated handler on an EventSource<int> is
// therefore rejected too; pre-fix it compiled and printed `0` for `Fire(7)`
// (the raw `int` bits read as a double — silent wrong code).
BEZEL_TEST(eventsource_handler_annotation_mismatch_4869,
           plus_eq_double_annotation_on_int_source_rejected_spec_27_1_1) {
    auto r = runAot(beeperProgram(
        "_beep += lambda(n: double) => Console.WriteLine(n);", "int", "7"));
    const auto* d = findDiag4869(r, kNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(d->message.find("annotated `double`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §27.1.1 first bullet: a class/interface parameter slot is CONTRAVARIANT — an
// `(Animal) -> ()` handler is usable where `(Dog) -> ()` is expected. The
// check must admit it (and it runs: `Animal.V` is 5, printed + 1).
BEZEL_TEST(eventsource_handler_annotation_mismatch_4869,
           plus_eq_base_class_annotation_contravariant_runs_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "class Animal public V: int := 5; end Animal; "
        "class Dog: Animal end Dog; "
        "class Beeper "
        "  private owned _beep: EventSource<Dog>; "
        "  public constructor() "
        "    _beep := new EventSource<Dog>(); "
        "    _beep += lambda(a: Animal) => Console.WriteLine(a.V + 1); "
        "  end constructor; "
        "  public method Fire(d: Dog) _beep.Raise(d); end Fire; "
        "end Beeper; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Beeper(); "
        "    using var d := new Dog(); "
        "    b.Fire(d); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(findDiag4869(r, "does not accept the event payload") ==
                      nullptr);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}
