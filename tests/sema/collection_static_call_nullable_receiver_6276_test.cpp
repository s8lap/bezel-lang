// #6276 (§7.4, stdlib §4.4, v0.338): the #3345 static-call sugar
// `Array.Sort(a)` / `Array.Reverse(a)` / `Array.Fill(a, v)` /
// `Array.CopyTo(a, b)` is DEFINED as the equivalent INSTANCE call on the first
// argument (`Array.Sort(a)` ↔ `a.Sort()`), so §7.4's member-access rule reaches
// it unchanged: the first argument IS the receiver. The sugar's guard tested
// only the receiver head name and the argument's arity, never its NULLABILITY,
// so the sugar accepted a program its own definition rejects.
//
// MEASURED at 389ca74b (staging tip) before the fix:
//   var n: Array<int>? := [3, 1]; Array.Sort(n);   -> compiles rc 0
//   var n: Array<int>? := [3, 1]; n.Sort();        -> rejected, §7.4
// The guard is `isNarrowedNonNullRead`, NOT `!argTy->nullable`: exprType keeps
// nullable = true on a §7.4-NARROWED identifier too, so a `!nullable` guard
// would wrongly reject the valid `if n <> null then Array.Sort(n); end`. The
// rows below cover un-narrowed, narrowed, force-unwrapped, and non-nullable
// reads, on every one of the four in-place members.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// The §7.4 diagnostic this fix introduces, quoted from the source that emits
// it. A rename here is a behavior flip for every row below.
const char* const kNarrowNeedle =
    "it is read at type `Array?` — a nullable value requires narrowing it to "
    "non-null first (§7.4)";

std::string compileAndGatherErrors(const std::string& body) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n" + body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) all += d.message + "\n";
    return all;
}

// An un-narrowed nullable receiver must be REJECTED with the §7.4 diagnostic
// — not merely "it did not compile", which a crash or a null deref would also
// satisfy, and not with some unrelated error.
void expectRejected(const std::string& body) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n" + body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kNarrowNeedle) != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// A read that IS non-null at the point of use must still compile and RUN.
void expectRuns(const std::string& body, const std::string& expected) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n" + body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

const std::string kNullableArray =
    "    var n: Array<int>? := [3, 1];\n";
const std::string kNonNullArray =
    "    var n: Array<int> := [3, 1];\n";

}  // namespace

// s7_4_0101: the issue's exact repro. §7.4's member-access rule applies
// unchanged because the sugar's first argument is the receiver.
BEZEL_TEST(collection_static_call_nullable_receiver_6276,
           un_narrowed_nullable_receiver_rejected_spec_7_4) {
    expectRejected(kNullableArray + "    Array.Sort(n);\n");
}

// s7_4_0102: §7.4 narrows the read inside `if n <> null`, so the sugar must
// accept it. This is the row a naive `!argTy->nullable` guard breaks —
// `exprType` keeps nullable = true on a narrowed identifier.
BEZEL_TEST(collection_static_call_nullable_receiver_6276,
           narrowed_receiver_accepted_spec_7_4) {
    expectRuns(kNullableArray +
                   "    if n <> null then\n"
                   "      Array.Sort(n);\n"
                   "    end;\n"
                   "    Console.WriteLine(\"ok\");\n",
               "ok\n");
}

// s7_4_0103: the `!` force-unwrap is the other §7.4 non-null read, and the
// stdlib's own remedy text names it. The program also proves the call actually
// ran and sorted: [3,1] -> [1,3] = 13.
BEZEL_TEST(collection_static_call_nullable_receiver_6276,
           force_unwrapped_receiver_accepted_and_sorts_spec_7_4) {
    expectRuns(kNullableArray +
                   "    Array.Sort(n!);\n"
                   "    Console.WriteLine(n!.Get(0).ToString());\n",
               "1\n");
}

// s7_4_0104: a receiver that is never nullable is untouched — the pre-#6276
// behaviour, and the whole point of the fix being narrow. [3,1] -> [1,3] = 13.
BEZEL_TEST(collection_static_call_nullable_receiver_6276,
           non_nullable_receiver_still_accepted_spec_7_4) {
    expectRuns(kNonNullArray +
                   "    Array.Sort(n);\n"
                   "    Console.WriteLine(n.Get(0).ToString());\n",
               "1\n");
}

// s7_4_0105: the guard is on the SUGAR, which covers all four in-place
// members #3345 admits (the copy forms `Array.Sorted()` / `Array.Reversed()`
// are arity-0 in this position and never reach the arm). Each is rejected
// identically, so the fix is not Sort-specific.
BEZEL_TEST(collection_static_call_nullable_receiver_6276,
           every_in_place_sugar_member_rejected_spec_7_4_stlib_4_4) {
    expectRejected(kNullableArray + "    Array.Sort(n);\n");
    expectRejected(kNullableArray + "    Array.Reverse(n);\n");
    expectRejected(kNullableArray + "    Array.Fill(n, 7);\n");
    expectRejected(kNullableArray +
                   "    var m: Array<int> := [1];\n"
                   "    Array.CopyTo(n, m);\n");
}

// s7_4_0106: a NON-collection nullable argument is NOT this diagnostic's
// business — the #6265 arm owns a static call whose argument is not a
// collection and reports "'Array' has no static method or property 'Sort'
// (§3.4)". The new guard must not pre-empt it: it keys on the argument being
// NULLABLE, and `string?` is nullable, so this row pins that the ordering
// between the two diagnostics is unchanged.
BEZEL_TEST(collection_static_call_nullable_receiver_6276,
           non_collection_argument_keeps_the_3_4_diagnostic_spec_3_4) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    var n: string? := \"x\";\n"
                    "    Array.Sort(n);\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("'Array' has no static method or property 'Sort' "
                           "(§3.4)") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// s7_4_0108: a bare `null` literal argument keeps the §3.4 diagnostic. §14.5:
// "a bare `null` literal argument is not self-determined", and §7.4's narrowing
// can never apply to it — so §7.4 is the wrong message for it, and the #6265
// arm ("'Array' has no static method or property 'Sort' (§3.4)") is the right
// one. This row guards the gate in the fix: without the same-family gate, the
// §7.4 diagnostic pre-empted §3.4 here.
BEZEL_TEST(collection_static_call_nullable_receiver_6276,
           bare_null_argument_keeps_the_3_4_diagnostic_spec_3_4_14_5) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    Array.Sort(null);\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("'Array' has no static method or property 'Sort' "
                           "(§3.4)") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// s7_4_0107: a §7.4-narrowed receiver through the `?.` NULL-CONDITIONAL
// spelling of the sugar is a different shape (`Array.Sort` is a static member
// on the type name, so `?.` does not apply to it) and is left alone; what this
// row pins is the surrounding truth that a narrowed read through the guard
// still compiles when the collection is read more than once, so the guard is
// consulted per-READ and not latched by the first one.
BEZEL_TEST(collection_static_call_nullable_receiver_6276,
           guard_consulted_per_read_not_latched_spec_7_4) {
    expectRuns(kNullableArray +
                   "    if n <> null then\n"
                   "      Array.Sort(n);\n"
                   "      Array.Reverse(n);\n"
                   "    end;\n"
                   "    Console.WriteLine(\"ok\");\n",
               "ok\n");
}