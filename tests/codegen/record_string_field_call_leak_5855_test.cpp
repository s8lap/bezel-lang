// A record local holding a RUNTIME-BUILT string leaks that string once the
// record is used in a call (§9.1 / §9.7 / §28.3 / §28.9, v0.357; #5855).
//
// §9.1 gives a record value semantics and §9.7 says a record has "no
// disposer": its `string` fields are bit-copied, never retained, when it is
// passed by value. The compiler releases such a record's field strings at
// the local's scope end (`collectRecordCallResultReleaseEligible`) only when
// the escape walk proves no copy of the record outlives the local. Before
// this fix the walk read four call-bounded uses as ESCAPES, so the release
// never armed and the string leaked once per call:
//   * the record as the RECEIVER of its synthesized `Equals` / `GetHashCode`;
//   * the record as the ARGUMENT of a synthesized `Equals`;
//   * the record passed BY VALUE to a method that only reads it;
//   * a named `string` local passed to the record's synthesized positional
//     constructor, which stores it RAW (the #4254 fast path), so neither the
//     local's release nor the record's armed (the #5853 T1 shape, #8630).
//
// MEASURED (bezelc --sanitize=address, r30-c1 4d689965e): every program below
// leaks on the base (LeakSanitizer: 3590 B in 200 allocations over the 100
// iterations); with the fix each is clean. Every program loops 100x so a
// single leaked object cannot hide behind a stale stack word (#8630).
//
// The twins that must STILL keep the string (the callee stores the record in
// a field, returns it, adds it to an array, captures it in a lambda, stores
// a field string, or passes it on to such a callee; a reassigned source
// local; a user-declared `Equals`) were probed 200x under ASan on both trees:
// none frees early (the lane handback lists them).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// The generated-code ASan oracle: the program must run, exit 0 (LeakSanitizer
// turns a leak into a non-zero exit) and print `expected`.
void expectCleanRun(const std::string& program, const std::string& expected) {
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

std::string loopProgram(const std::string& helpers, const std::string& once) {
    return "module M; record Name(value: string, n: int); "
           "interface IFoo method Get(): int; end IFoo; "
           "record NamedI(value: string, n: int): IFoo "
           "  public method Get(): int => n; "
           "end NamedI; "
           "class C " +
           helpers +
           "  static method Once(i: int): int " + once +
           "  end Once; "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<100 do t := t + Once(i); end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end C;";
}
}  // namespace

// §9.1 / §8.4: the synthesized `Equals` compares fields and keeps neither
// operand, so the receiver's field string is released at its scope end.
BEZEL_TEST(record_string_field_call_leak_5855,
           synthesized_equals_receiver_releases_string_spec_9_1_s28_9) {
    expectCleanRun(loopProgram("",
                               "var a := new Name(\"x\" + i.ToString(), 1); "
                               "var b := new Name(\"bye\", 2); "
                               "var r := a.Equals(b); "
                               "return a.n;"),
                   "100\n");
}

// §9.1 / §8.4: the same, for the record as the `Equals` ARGUMENT.
BEZEL_TEST(record_string_field_call_leak_5855,
           synthesized_equals_argument_releases_string_spec_9_1_s28_9) {
    expectCleanRun(loopProgram("",
                               "var a := new Name(\"x\" + i.ToString(), 1); "
                               "var b := new Name(\"bye\", 2); "
                               "var r := b.Equals(a); "
                               "return a.n;"),
                   "100\n");
}

// §9.1: the synthesized `GetHashCode` hashes fields and keeps the receiver.
BEZEL_TEST(record_string_field_call_leak_5855,
           synthesized_gethashcode_receiver_releases_string_spec_9_1_s28_9) {
    expectCleanRun(loopProgram("",
                               "var a := new Name(\"x\" + i.ToString(), 1); "
                               "var h := a.GetHashCode(); "
                               "return a.n;"),
                   "100\n");
}

// §9.7 / §28.3: a by-value pass to a method that only reads scalar and
// string fields (and passes the record on to another such method) does not
// let the record outlive the call.
BEZEL_TEST(record_string_field_call_leak_5855,
           by_value_pass_to_reading_callee_releases_string_spec_9_7_s28_3) {
    expectCleanRun(
        loopProgram("  static method Read(p: Name): int "
                    "    => (if p.value.Length > 0 then p.n else 0); "
                    "  static method Fwd(p: Name): int => Read(p); ",
                    "var a := new Name(\"x\" + i.ToString(), 1); "
                    "return Fwd(a);"),
        "100\n");
}

// §9.1 / §28.9: a named string local handed to the synthesized positional
// constructor — the record borrows it, so the local's own release fires.
// (#8630's `h2_local_into_record` probe.)
BEZEL_TEST(record_string_field_call_leak_5855,
           string_local_into_positional_ctor_released_spec_9_1_s28_9) {
    expectCleanRun(loopProgram("",
                               "var t := \"x\" + i.ToString(); "
                               "var n := new Name(t, 1); "
                               "return n.n;"),
                   "100\n");
}

// §8.6 / §28.3: the same construction, the record then boxed into a
// non-escaping interface parameter (the #5853 T1 shape).
BEZEL_TEST(record_string_field_call_leak_5855,
           string_local_record_to_interface_param_released_spec_8_6_s28_3) {
    expectCleanRun(
        loopProgram("  static method TakeI(f: IFoo): int => f.Get(); ",
                    "var t := \"x\" + i.ToString(); "
                    "var n := new NamedI(t, 1); "
                    "return TakeI(n);"),
        "100\n");
}

// §21.1 / §9.1 / §28.9: the `const` spellings of the constructor-borrow shape.
// A §21.1 runtime `const` is a different AST node (`LocalConstDecl`) from a
// `var`, and `collectCtorRecordBorrowEligible` read only `LocalDecl`, so the
// `var` twin above was clean while each of these leaked the string per call.
BEZEL_TEST(record_string_field_call_leak_5855,
           const_string_local_into_positional_ctor_released_spec_21_1_s28_9) {
    expectCleanRun(loopProgram("",
                               "const t := \"x\" + i.ToString(); "
                               "var n := new Name(t, 1); "
                               "return n.n;"),
                   "100\n");
}

BEZEL_TEST(record_string_field_call_leak_5855,
           string_local_into_const_record_released_spec_21_1_s28_9) {
    expectCleanRun(loopProgram("",
                               "var t := \"x\" + i.ToString(); "
                               "const n := new Name(t, 1); "
                               "return n.n;"),
                   "100\n");
}

BEZEL_TEST(record_string_field_call_leak_5855,
           const_string_local_into_const_record_released_spec_21_1_s28_9) {
    expectCleanRun(loopProgram("",
                               "const t := \"x\" + i.ToString(); "
                               "const n := new Name(t, 1); "
                               "return n.n;"),
                   "100\n");
}
