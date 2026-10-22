// Codegen regression tests for issue #4610: §35.3 *Construction and payload
// ownership* + §7.5 *Nullable value types*, spec v0.306.
//
// THE BUG. Constructing a choice case whose payload is declared with a
// NULLABLE VALUE type aborted the compiler outright:
//
//     module N5;
//     choice Opt S(v: int?), N(); end Opt;
//     class C public static method Main(): int
//       var o: Opt := Opt.S(7);
//       return 0;
//     end Main; end C;
//
//     $ bezelc n5.bzl -o n5.x
//     LLVM ERROR: Invalid size request on a scalable vector.
//     Aborted (core dumped)          # exit 134
//
// Six lines, no generics, no `owned`. §35.4 rule 3 classifies `Opt`
// **value-like** — "every payload is a value … an ordinary copyable value
// type" — and §7.5 makes `int?` an ordinary nullable value type, so nothing in
// §35.1's payload rules excludes it. The program is spec-legal and must run.
//
// THE CAUSE. §35.9 / §15.1 lay a nullable value out as the tagged struct
// `{ i8 hasValue, T value }`, and `choiceStruct` gives the payload slot that
// element type — but the payload ARGUMENT lowers to the bare inner value
// (`i32 7`). The choice-case construction inserted the bare value into the
// tagged element without the §15.1 wrap, and LLVM's constant folder turned the
// mistyped InsertValue into a malformed constant aggregate,
// `{ { i8, i32 } } { i32 7 }`. Nothing on the codegen path rejects it
// (`verifyModule` is opt-in), so it reached the backend, which measured the
// mistyped element and called `report_fatal_error`.
//
// THE ORACLE. §35.5's borrowing match reads the payload back, so each row
// pins the VALUE the program computes, not "it compiled". That matters here:
// the pre-fix behaviour is a compiler ABORT, so
// `failedAtCompileTimeWithRealLocation` would be vacuous (the backstop
// diagnostic carries a real location on both sides) and even "it compiled"
// would only pin half the fix — a wrongly-tagged `hasValue` byte compiles
// fine and reads back as the wrong nullability. `r.ran` is asserted alongside
// the value because the fork-isolated AOT harness reports the pre-fix abort as
// `ran == false`.
//
// The two CONTROL rows are the issue's own discriminating grid: the case with
// the nullable payload never constructed (`Opt.N()`), and a nullable
// REFERENCE payload (`string?`, a bare pointer — no tagged struct, hence no
// wrap to miss). Both were clean before the fix and must stay clean after it,
// which is what makes "a constructed nullable VALUE payload slot" the whole
// difference.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Wrap declarations + a method body in the standard module/class shell and
// run it. Returns the harness result so each row can assert `ran` as well as
// the value — a compiler abort is `ran == false`, not a wrong exit code.
bezel::test::JitResult run(const std::string& decls, const std::string& body) {
    return runAot(std::string("module M; ") + decls +
                  " class Host public static method Main(): int " + body +
                  " end Main; end Host;");
}

void expectValue(const bezel::test::JitResult& r, int expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, expected);
}

}  // namespace

// --- THE BUG: a constructed nullable VALUE payload --------------------------

BEZEL_TEST(codegen_choice_nullable_value_payload_construct_4610,
           spec_35_3_nullable_int_payload_constructed_and_read_back) {
    // The issue's repro, extended with §35.5's borrowing match so the row pins
    // the VALUE and not merely that codegen terminated. Pre-fix: `LLVM ERROR:
    // Invalid size request on a scalable vector.` / SIGABRT.
    expectValue(
        run("choice Opt S(v: int?), N(); end Opt;",
            "var o: Opt := Opt.S(7); "
            "var r := 0; "
            "case o of "
            "  S(var v) => do r := (if v is not null then v! else -1); end, "
            "  N()      => do r := -2; end "
            "end case; "
            "return r;"),
        7);
}

BEZEL_TEST(codegen_choice_nullable_value_payload_construct_4610,
           spec_7_5_null_argument_to_a_nullable_value_payload_reads_as_null) {
    // The other half of §7.5: `Opt.S(null)` must tag the slot `hasValue = 0`.
    // MEASURED clean on BOTH sides of the fix — a `null` literal already lowered
    // to the all-zero tagged struct, so the missing wrap had nothing to
    // mistype — which makes this a shape constraint on the fix rather than a
    // reproduction of the bug: a fix that wrote the payload bytes but left the
    // tag undefined would pass the row above and fail this one.
    expectValue(
        run("choice Opt S(v: int?), N(); end Opt;",
            "var o: Opt := Opt.S(null); "
            "var r := 0; "
            "case o of "
            "  S(var v) => do r := (if v is not null then v! else 99); end, "
            "  N()      => do r := -2; end "
            "end case; "
            "return r;"),
        99);
}

BEZEL_TEST(codegen_choice_nullable_value_payload_construct_4610,
           spec_35_3_nullable_payload_beside_plain_value_payloads) {
    // §35.3 binds payload arguments "as to a parameter list", in declaration
    // order. The wrap is applied per payload FIELD, so a nullable slot between
    // two plain ones must leave its neighbours' slots untouched: 3 + 7 + 11.
    expectValue(
        run("choice Opt S(a: int, v: int?, b: int), N(); end Opt;",
            "var o: Opt := Opt.S(3, 7, 11); "
            "var r := 0; "
            "case o of "
            "  S(var a, var v, var b) => do "
            "    r := a + b + (if v is not null then v! else -1); end, "
            "  N() => do r := -2; end "
            "end case; "
            "return r;"),
        21);
}

BEZEL_TEST(codegen_choice_nullable_value_payload_construct_4610,
           spec_35_11_generic_nullable_value_payload_at_int_instantiation) {
    // §35.11: "classification is per-instantiation". `choice Opt<T> S(v: T?)`
    // at `Opt<int>` substitutes to the same tagged `{ i8, i32 }` payload slot
    // and aborted identically — the issue's fourth grid row, reduced to the
    // part that needs no `owned`.
    expectValue(
        run("choice Opt<T> S(v: T?), N(); end Opt;",
            "var o: Opt<int> := Opt<int>.S(7); "
            "var r := 0; "
            "case o of "
            "  S(var v) => do r := (if v is not null then v! else -1); end, "
            "  N()      => do r := -2; end "
            "end case; "
            "return r;"),
        7);
}

// --- CONTROLS: clean before the fix, and must stay clean --------------------

BEZEL_TEST(codegen_choice_nullable_value_payload_construct_4610,
           control_nullable_value_case_never_constructed_is_unaffected) {
    // The issue's second grid row: declaring the nullable payload is harmless;
    // only CONSTRUCTING that case reached the mistyped InsertValue. This row
    // passed on both sides of the fix and is here to keep it that way.
    expectValue(
        run("choice Opt S(v: int?), N(); end Opt;",
            "var o: Opt := Opt.N; "
            "var r := 0; "
            "case o of "
            "  S(var v) => do r := -1; end, "
            "  N()      => do r := 5; end "
            "end case; "
            "return r;"),
        5);
}

BEZEL_TEST(codegen_choice_nullable_value_payload_construct_4610,
           control_nullable_reference_payload_needs_no_wrap) {
    // The issue's third grid row: a nullable REFERENCE payload (`string?`) is
    // a bare pointer under §7.5's reference spelling — no `{ i8, T }` struct,
    // so no wrap to miss — and compiled cleanly before the fix. It proves the
    // defect is specific to a nullable VALUE slot rather than to `?` at all.
    expectValue(
        run("choice Opt S(v: string?), N(); end Opt;",
            "var o: Opt := Opt.S(\"xy\"); "
            "var r := 0; "
            "case o of "
            "  S(var v) => do r := (if v is not null then v!.Length else -1); "
            "              end, "
            "  N()      => do r := -2; end "
            "end case; "
            "return r;"),
        2);
}
