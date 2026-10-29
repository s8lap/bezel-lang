// Fix #7342 -- a §7.4-NARROWED `Guid?` receiver overflowed the stack at
// `x.ToString("D")` / `x.ToString()` (spec §7.4 and §22.6, stdlib §2.6, v0.350).
//
// THE DEFECT. spec §22.6: "a read of a binding §7.4 has narrowed non-null is of
// its non-nullable type", and §7.4 makes a member call on a nullable value legal
// only once the compiler has narrowed it. So `x.ToString("D")` over a narrowed
// `x: Guid?` is a call on a `Guid`. The `Guid` instance-method arm lowered the
// receiver but never took the payload out of the `{i8 hasValue; Guid}` carrier
// the read still loads: it stored the whole carrier into a `Guid`-sized (16
// byte) stack slot -- an ASan stack-buffer-overflow, WRITE of size 20 -- and
// handed the runtime a Guid whose first byte was the hasValue flag.
//
// THE FIX. The arm extracts the payload of a nullable receiver before the slot
// store. The extraction is sound because §7.4's member-access gate refuses the
// UN-narrowed nullable receiver for every member (reject control below).
//
// ORACLE. `runAotChecked` (generated-code ASan). Every row asserts the program
// ran, its exit code and its EXACT stdout, so a compile-time rejection cannot
// pass as a memory-safe run.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return std::string(
               "module M; import System;\nclass C\n"
               "  public static method Mk(): Guid? =>\n"
               "    Guid.Parse(\"11111111-2222-3333-4444-555555555555\");\n"
               "  public static method NoneG(): Guid? => null;\n"
               "  public static method Main(): int\n") +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

std::string stdoutOf(const JitResult& r) {
    std::string t = r.stdoutText;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    return t;
}

}  // namespace

// The issue's repro, one-argument form: `x.ToString("D")`.
BEZEL_TEST(guid_narrowed_nullable_receiver_7342, to_string_format_d_spec_7_4) {
    JitResult r = runAotChecked(prog(
        "    var x: Guid? := C.Mk();\n"
        "    if x <> null then\n"
        "      Console.WriteLine(x.ToString(\"D\"));\n"
        "    end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(stdoutOf(r), "11111111-2222-3333-4444-555555555555");
}

// The issue's repro, no-argument form: `x.ToString()`.
BEZEL_TEST(guid_narrowed_nullable_receiver_7342, to_string_no_arg_spec_7_4) {
    JitResult r = runAotChecked(prog(
        "    var x: Guid? := C.Mk();\n"
        "    if x <> null then\n"
        "      Console.WriteLine(x.ToString());\n"
        "    end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(stdoutOf(r), "11111111-2222-3333-4444-555555555555");
}

// The issue's literal repro with `Guid.Empty` as the initializer.
BEZEL_TEST(guid_narrowed_nullable_receiver_7342, empty_initializer_spec_7_4) {
    JitResult r = runAotChecked(prog(
        "    var x: Guid? := Guid.Empty;\n"
        "    if x <> null then\n"
        "      Console.WriteLine(x.ToString(\"D\"));\n"
        "      Console.WriteLine(x.ToString());\n"
        "    end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    "00000000-0000-0000-0000-000000000000\n"
                    "00000000-0000-0000-0000-000000000000");
}

// The "N" specifier and the other receiver members share the arm: GetHashCode
// agrees with the non-nullable Guid's, Equals/CompareTo take the narrowed
// receiver's payload.
BEZEL_TEST(guid_narrowed_nullable_receiver_7342, other_members_on_narrowed_receiver_spec_7_4) {
    JitResult r = runAotChecked(prog(
        "    var x: Guid? := C.Mk();\n"
        "    var g: Guid := Guid.Parse(\"11111111-2222-3333-4444-555555555555\");\n"
        "    if x <> null then\n"
        "      Console.WriteLine(x.ToString(\"N\"));\n"
        "      Console.WriteLine(x.Equals(g));\n"
        "      Console.WriteLine(x.CompareTo(g));\n"
        "      Console.WriteLine(x.GetHashCode() = g.GetHashCode());\n"
        "    end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    "11111111222233334444555555555555\nTrue\n0\nTrue");
}

// A narrowed `Guid?` PARAMETER receiver (a different slot kind than a local).
BEZEL_TEST(guid_narrowed_nullable_receiver_7342, narrowed_parameter_receiver_spec_7_4) {
    JitResult r = runAotChecked(std::string(
        "module M; import System;\nclass C\n"
        "  public static method Show(g: Guid?): string\n"
        "    if g <> null then return g.ToString(\"D\"); end;\n"
        "    return \"none\";\n"
        "  end Show;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(C.Show(Guid.Parse(\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\")));\n"
        "    Console.WriteLine(C.Show(null));\n"
        "    return 0;\n"
        "  end Main;\nend C;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(stdoutOf(r), "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\nnone");
}

// CONTROL: a NON-nullable `Guid` receiver renders as before.
BEZEL_TEST(guid_narrowed_nullable_receiver_7342, non_nullable_receiver_control_spec_2_6) {
    JitResult r = runAotChecked(prog(
        "    var g: Guid := Guid.Parse(\"11111111-2222-3333-4444-555555555555\");\n"
        "    Console.WriteLine(g.ToString(\"D\"));\n"
        "    Console.WriteLine(g.ToString());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    "11111111-2222-3333-4444-555555555555\n"
                    "11111111-2222-3333-4444-555555555555");
}

// REJECT CONTROL (§7.4): the un-narrowed nullable receiver stays a compile
// error, which is what makes the payload extraction sound.
BEZEL_TEST(guid_narrowed_nullable_receiver_7342, unnarrowed_receiver_rejected_spec_7_4) {
    JitResult r = runAotChecked(prog(
        "    var x: Guid? := C.NoneG();\n"
        "    Console.WriteLine(x.ToString(\"D\"));"));
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool said = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("on a nullable value requires narrowing it to "
                           "non-null first (§7.4)") != std::string::npos)
            said = true;
    BEZEL_EXPECT_TRUE(said);
}
