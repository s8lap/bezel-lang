// Regression tests for #7565 -- a temporary `new StringBuilder(...)` used only
// as a call or property receiver was never disposed and leaked one parts array
// per evaluation. (An argument to a plain parameter is NOT claimed: the callee
// may return or store it, which sema admits today; see the soundness rows.)
//
// Language spec §28 K8.1 (v0.350), verbatim: "Unbound owned temporaries are
// disposed at their full expression's end, after their uses, in reverse
// production order -- on the normal edge as on the exception edge, where the
// pass runs during unwinding." §28.3 (v0.350) names the same obligation for a
// non-`owned` argument position. stdlib §3.1: `Append` / `Insert` / `Remove` /
// `Replace` / `Clear` return the builder (a borrow of the receiver), `Length`
// is an `int`, `ToString()` a fresh string.
//
// Every leak row runs on runAotChecked (ASan + LSan) and loops 100 times: a
// one-shot repro is a vacuous oracle for a leak. The SOUNDNESS rows at the end
// pin the gate: a builder that can outlive the boundary (a chain's `Append`
// result, a user method returning its `StringBuilder` parameter, or a callee
// storing it in `this`, in static storage or in another argument) must not be
// disposed there, or the binding / returned / stored value dangles. They run
// on runAotCheckedLenient: generated-code ASan stays ON (a heap-use-after-free
// fails the row), LeakSanitizer is OFF (those shapes keep their pre-fix leak,
// the sound direction, until sema rejects the escaping borrow).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotCheckedLenient;
using bezel::test::runAotChecked;

namespace {

std::string loop100Src(const std::string& body, const std::string& extraMembers,
                       const std::string& prelude) {
    return "module M;\n" + prelude +
        "class C\n" +
        extraMembers +
        "  public static method Main(): int\n"
        "    var tot := 0;\n"
        "    for var i in 0..<100 do\n" +
        body +
        "    end;\n"
        "    Console.WriteLine(tot);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

JitResult loop100(const std::string& body, const std::string& extraMembers = "",
                  const std::string& prelude = "") {
    return runAotChecked(loop100Src(body, extraMembers, prelude));
}

// Generated-code ASan ON (a use-after-free fails the row), LSan OFF.
JitResult loop100NoLeakCheck(const std::string& body,
                             const std::string& extraMembers = "",
                             const std::string& prelude = "") {
    return runAotCheckedLenient(loop100Src(body, extraMembers, prelude));
}

void expectClean(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           chained_append_tostring_receiver_loop_100_spec_28_k8_1) {
    // "abxy".Length = 4, 100 times.
    expectClean(
        loop100("      tot := tot + new StringBuilder(\"ab\").Append(\"x\")"
                ".Append(\"y\").ToString().Length;\n"),
        "400\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           length_property_receiver_loop_100_spec_28_k8_1) {
    expectClean(loop100("      tot := tot + new StringBuilder(\"zz\").Length;\n"),
                "200\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           chained_length_property_receiver_loop_100_spec_28_k8_1) {
    expectClean(
        loop100("      tot := tot + new StringBuilder(\"zz\").Append(\"q\").Length;\n"),
        "300\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           insert_remove_replace_receiver_loop_100_spec_28_k8_1) {
    // "abc" -> Insert(0,"q") "qabc" -> Remove(1,2) "qc" -> Replace("c","ZZ")
    // "qZZ": Length 3, 100 times.
    expectClean(
        loop100("      var t := new StringBuilder(\"abc\").Insert(0, \"q\")"
                ".Remove(1, 2).Replace(\"c\", \"ZZ\").ToString();\n"
                "      tot := tot + t.Length;\n"),
        "300\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           expression_statement_chain_loop_100_spec_28_k8_1) {
    // The statement discards the returned builder: nothing can hold it.
    expectClean(
        loop100("      new StringBuilder(\"abc\").Append(\"d\");\n"
                "      tot := tot + 1;\n"),
        "100\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           receiver_in_condition_is_per_evaluation_loop_100_spec_28_k8_2) {
    // K8.2: a condition is a per-evaluation boundary.
    expectClean(
        loop100("      var k := 0;\n"
                "      while new StringBuilder(\"q\").Length > k do k := k + 1; end;\n"
                "      tot := tot + k;\n"),
        "100\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           exception_in_later_operand_disposes_temp_loop_100_spec_28_k8_1) {
    // K8.1: the exception edge disposes the produced temporary too.
    expectClean(
        loop100("      try\n"
                "        tot := tot + new StringBuilder(\"ab\").Append(C.Boom())"
                ".ToString().Length;\n"
                "      catch\n"
                "        Exception => tot := tot + 1;\n"
                "      end try;\n",
                "  public static method Boom(): string\n"
                "    throw new Exception(\"x\");\n"
                "  end Boom;\n"),
        "100\n");
}

// SOUNDNESS rows -- runAotCheckedLenient (see the header). Each shape's result
// can be the builder, so disposing the argument / root at the boundary would
// free the parts array the caller still reads: ASan reports it as a
// heap-use-after-free and the row fails.
BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           result_aliasing_the_builder_is_not_disposed_spec_28_3) {
    // Run 50x under generated-code ASan: `b` is the chain's own builder.
    JitResult r = runAotCheckedLenient(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var tot := 0;\n"
        "    for var i in 0..<50 do\n"
        "      var b := new StringBuilder(\"a\").Append(\"xy\");\n"
        "      tot := tot + b.Length;\n"
        "      Console.WriteLine(new StringBuilder(\"w\").Append(\"v\"));\n"
        "    end;\n"
        "    Console.WriteLine(tot);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    std::string expect;
    for (int i = 0; i < 50; ++i) expect += "wv\n";
    expect += "150\n";
    expectClean(r, expect.c_str());
}

namespace {
const char* kIdStatic =
    "  public static method Id(b: StringBuilder): StringBuilder => b;\n";
}  // namespace

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           static_identity_result_returned_is_not_disposed_r04d_spec_28_3) {
    // `return C.Id(new StringBuilder("abc"));` -- the returned value IS the
    // argument; disposing it at Ret's scope end dangles it in Main.
    expectClean(
        loop100NoLeakCheck(
            "      tot := tot + C.Ret().Length;\n",
            std::string(kIdStatic) +
                "  public static method Ret(): StringBuilder\n"
                "    return C.Id(new StringBuilder(\"abc\"));\n"
                "  end Ret;\n"),
        "300\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           static_identity_result_bound_then_returned_r04e_spec_28_3) {
    expectClean(
        loop100NoLeakCheck(
            "      tot := tot + C.Ret().Length;\n",
            std::string(kIdStatic) +
                "  public static method Ret(): StringBuilder\n"
                "    var x := C.Id(new StringBuilder(\"abc\"));\n"
                "    return x;\n"
                "  end Ret;\n"),
        "300\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           static_identity_result_bound_mutated_returned_r04f_spec_28_3) {
    // Ret: "abczz" then Main appends "y" -> 6; Ret2: "abczz".Length 5.
    // 11 per iteration, 100 times.
    expectClean(
        loop100NoLeakCheck(
            "      var r := C.Ret();\n"
            "      r.Append(\"y\");\n"
            "      tot := tot + r.Length + C.Ret2();\n",
            std::string(kIdStatic) +
                "  public static method Ret(): StringBuilder\n"
                "    var x := C.Id(new StringBuilder(\"abc\"));\n"
                "    x.Append(\"zz\");\n"
                "    return x;\n"
                "  end Ret;\n"
                "  public static method Ret2(): int\n"
                "    var x := C.Id(new StringBuilder(\"abc\"));\n"
                "    x.Append(\"zz\");\n"
                "    return x.Length;\n"
                "  end Ret2;\n"),
        "1100\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           instance_identity_result_returned_is_not_disposed_r22_spec_28_3) {
    JitResult r = runAotCheckedLenient(
        "module M;\n"
        "class K\n"
        "  public constructor() end constructor;\n"
        "  public method Id(b: StringBuilder): StringBuilder => b;\n"
        "end K;\n"
        "class C\n"
        "  public static method Ret(k: K): StringBuilder\n"
        "    return k.Id(new StringBuilder(\"abc\"));\n"
        "  end Ret;\n"
        "  public static method Main(): int\n"
        "    var tot := 0;\n"
        "    var k := new K();\n"
        "    for var i in 0..<100 do\n"
        "      tot := tot + C.Ret(k).Length;\n"
        "    end;\n"
        "    Console.WriteLine(tot);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectClean(r, "300\n");
}

// A callee that STORES its plain parameter (§28.3 v0.350, "A callee that
// stores a plain argument"): sema admits all three stores below, so the
// builder is read after the call. Claiming the argument at the call turned the
// pre-fix leak into a heap-use-after-free on each (MEASURED); the plain-
// parameter argument is therefore not claimed at all.
BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           instance_callee_stores_param_in_this_field_r03c_spec_28_3) {
    JitResult r = runAotCheckedLenient(
        "module M;\n"
        "class H\n"
        "  public b: StringBuilder?;\n"
        "  public constructor() end constructor;\n"
        "  public method Keep(x: StringBuilder) this.b := x; end Keep;\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var tot := 0;\n"
        "    var h := new H();\n"
        "    for var i in 0..<100 do\n"
        "      h.Keep(new StringBuilder(\"a\"));\n"
        "    end;\n"
        "    tot := h.b!.Length;\n"
        "    Console.WriteLine(tot);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectClean(r, "1\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           static_callee_stores_param_in_static_storage_spec_28_3) {
    // A mutable static of StringBuilder type is rejected (§22.12 rule 2); a
    // static readonly Array admits the store (with the rule 4 warning).
    JitResult r = runAotCheckedLenient(
        "module M;\n"
        "class S\n"
        "  public static readonly xs: Array<StringBuilder> := new Array<StringBuilder>();\n"
        "  public static method Keep(x: StringBuilder) S.xs.Add(x); end Keep;\n"
        "end S;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    for var i in 0..<100 do\n"
        "      S.Keep(new StringBuilder(\"ab\"));\n"
        "    end;\n"
        "    Console.WriteLine(S.xs[0].Length + S.xs[99].Length);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectClean(r, "4\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           static_callee_stores_param_in_other_argument_spec_28_3) {
    JitResult r = runAotCheckedLenient(
        "module M;\n"
        "class H\n"
        "  public b: StringBuilder?;\n"
        "  public constructor() end constructor;\n"
        "end H;\n"
        "class S\n"
        "  public static method Keep(h: H, x: StringBuilder) h.b := x; end Keep;\n"
        "end S;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var h := new H();\n"
        "    for var i in 0..<100 do\n"
        "      S.Keep(h, new StringBuilder(\"abc\"));\n"
        "    end;\n"
        "    Console.WriteLine(h.b!.Length);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectClean(r, "3\n");
}

// EXTENSION rows -- runAotCheckedLenient. A user extension on StringBuilder
// takes its receiver as a plain borrow (§26.7, v0.350: "The receiver is always
// a borrow") and may hand it to a plain parameter that stores it (§28.3,
// v0.350: "A non-`owned` argument position is a borrowing use whatever the
// callee's body does with the parameter"). The chain walk must not descend
// through such a call to claim the root `new`: `h.b` still holds the builder
// after the full expression, and the post-loop `Append` on it would be a
// heap-use-after-free. `Count` is the name that reaches the walk today (the
// §26.5 rule 2 decline happens before the receiver is lowered); the `Stash`
// row pins the same shape for a name that does not (issue #7703's double
// evaluation hides it until that is fixed).
namespace {

std::string extensionStoreSrc(const std::string& extName, const std::string& body,
                              const std::string& extra = "") {
    return "module M;\n"
        "class H\n"
        "  public b: StringBuilder?;\n"
        "  public constructor() end constructor;\n"
        "  public method Keep(x: StringBuilder) this.b := x; end Keep;\n"
        "end H;\n"
        "extension StringBuilder\n"
        "  public method " + extName + "(h: H): StringBuilder\n"
        "    h.Keep(this);\n"
        "    return this;\n"
        "  end " + extName + ";\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var h := new H();\n"
        "    var tot := 0;\n"
        "    for var i in 0..<100 do\n" +
        body +
        "    end;\n"
        "    h.b!.Append(\"z\");\n"
        "    Console.WriteLine(h.b!.ToString());\n"
        "    Console.WriteLine(tot);\n" +
        extra +
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

}  // namespace

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           extension_storing_receiver_then_tostring_is_not_disposed_e10_spec_26_7) {
    expectClean(
        runAotCheckedLenient(extensionStoreSrc(
            "Count",
            "      var s := new StringBuilder(\"a\").Append(\"b\").Count(h).ToString();\n"
            "      tot := tot + s.Length;\n")),
        "abz\n200\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           extension_storing_receiver_then_length_is_not_disposed_e10_spec_26_7) {
    expectClean(
        runAotCheckedLenient(extensionStoreSrc(
            "Count",
            "      tot := tot + new StringBuilder(\"a\").Append(\"b\").Count(h).Length;\n")),
        "abz\n200\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           extension_storing_receiver_whole_statement_is_not_disposed_e10_spec_26_7) {
    expectClean(
        runAotCheckedLenient(extensionStoreSrc(
            "Count",
            "      new StringBuilder(\"a\").Append(\"b\").Count(h);\n"
            "      tot := tot + 1;\n")),
        "abz\n100\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           extension_storing_receiver_other_name_is_not_disposed_e01_spec_26_7) {
    expectClean(
        runAotCheckedLenient(extensionStoreSrc(
            "Stash",
            "      var s := new StringBuilder(\"a\").Append(\"b\").Stash(h).ToString();\n"
            "      tot := tot + s.Length;\n")),
        "abz\n200\n");
}

// NAMED-ARGUMENT row -- runAotChecked (ASan + LSan), loop 100. A reordered
// named call pins its receiver before its arguments (§22.5, v0.350, see
// #7600), so the fresh root is produced BEFORE `index := Boom()` throws; K8.1
// (v0.350) disposes it "on the exception edge" too. It leaked one builder per
// throwing iteration when the registration waited for the StringBuilder arm.
BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           named_reordered_throwing_argument_disposes_root_loop_100_spec_28_k8_1) {
    expectClean(
        loop100("      try\n"
                "        tot := tot + new StringBuilder(\"abc\")"
                ".Insert(value := C.Ts(\"Z\"), index := C.Boom()).Length;\n"
                "      catch\n"
                "        InvalidOperationException => tot := tot + 1;\n"
                "      end try;\n",
                "  public static method Ts(v: string): string => v;\n"
                "  public static method Boom(): int\n"
                "    throw new InvalidOperationException(\"boom\");\n"
                "  end Boom;\n"),
        "100\n");
}

BEZEL_TEST(stringbuilder_fresh_receiver_temp_7565,
           named_reordered_argument_normal_edge_loop_100_spec_28_k8_1) {
    // "abc" -> Insert(1, "Z") "aZbc": Length 4; the Remove statement discards
    // its builder. Both run through the #7600 receiver pin.
    expectClean(
        loop100("      tot := tot + new StringBuilder(\"abc\")"
                ".Insert(value := C.Ts(\"Z\"), index := 1).Length;\n"
                "      new StringBuilder(\"abcdef\").Remove(length := 2, startIndex := 1);\n",
                "  public static method Ts(v: string): string => v;\n"),
        "400\n");
}
