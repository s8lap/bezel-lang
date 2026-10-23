// fix #7044 (§12.7 / §9.5 / §7.4, v0.340): a `with` over a §12.7-TYPE-
// narrowed source (`if c is P then … c with(…) end;` over `c: P?`) copies
// the TAGGED `{i8, P}` nullable carrier into the BARE record copy slot.
//
//   §12.7 (v0.340): "When the test succeeds and the tested expression is one
//   of §7.4's narrowing carriers, the carrier is flow-narrowed to `T` within
//   the controlled block" — `c is P` retypes the READ of `c` to `P`.
//   §7.4 (v0.340): the narrowing applies "inside the controlled block of a
//   successful `is` test".
//   §9.5 (v0.340): `with` produces a COPY of the record with the named fields
//   replaced; the source is unchanged.
//
// §12.7 retypes the READ, not the STORAGE: `c`'s slot is still the tagged
// `{i8, P}` carrier, because the carrier's DECLARED type is `P?`. Codegen's
// `lowerWith` decided "is this a nullable record source?" from the source's
// SEMANTIC type — which the narrowing had already made non-nullable — so the
// #1273 unwrap was skipped and the 24-byte tagged value was stored into the
// 16-byte bare-record copy alloca. MEASURED on c3c9a2d4: AddressSanitizer
// reports "stack-buffer-overflow ... WRITE of size 24" in `main`, at the
// `store { i8, %record.P } …, ptr %copy` in the `with` copy.
//
// The rows below assert the ANSWER (44), not merely the absence of a crash,
// and run through runAotChecked so the generated code is ASan-instrumented:
// the pre-fix overflow aborts the program, so the row fails on the memory
// error as well as on the value.

#include <iostream>
#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Run `src` under the ASan-instrumented AOT oracle and compare its stdout
// and exit status with `expected`. Prints the ACTUAL value on a mismatch —
// the harness's BEZEL_EXPECT_EQ prints the expression text, not the value.
void expectOut(const std::string& src, const std::string& expected) {
    auto r = runAotChecked(src);
    std::string got;
    if (!r.ran) {
        got = "DID NOT RUN:";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error) got += " | " + d.message;
    } else {
        got = r.stdoutText + "exit=" + std::to_string(r.exitCode);
    }
    if (got != expected) {
        std::cout << "    expected \"" << expected << "\", got \"" << got
                  << "\"\n";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error)
                std::cout << "    [diag] " << d.message << "\n";
    }
    BEZEL_EXPECT_EQ(got, expected);
}

}  // namespace

// The issue's literal repro (§12.7 narrowing + §9.5 `with`). Expected: 44.
BEZEL_TEST(codegen_with_type_narrowed_source_spec_12_7_9_5_7044,
           issue_repro_prints_44) {
    expectOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        d[\"a\"] := new P(\"a\", 5);\n"
        "        var c := d.GetOrNull(\"a\");\n"
        "        if c is P then\n"
        "            var r: P := c with(Stock := 44);\n"
        "            Console.WriteLine(r.Stock);\n"
        "        end;\n"
        "    end Main;\n"
        "end Program;\n",
        "44\nexit=0");
}

// The §9.5 copy keeps the fields the `with`-clause does NOT replace, and a
// PARAMETER carrier narrows exactly as a local does (§12.7: "Narrowing
// applies to the original variable" — a parameter is one).
BEZEL_TEST(codegen_with_type_narrowed_source_spec_12_7_9_5_7044,
           narrowed_parameter_source_keeps_other_field) {
    expectOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Bump(p: P?): P\n"
        "        if p is P then\n"
        "            var r: P := p with(Stock := 44);\n"
        "            return r;\n"
        "        end;\n"
        "        return new P(\"none\", 0);\n"
        "    end Bump;\n"
        "    public static method Main()\n"
        "        var r := Bump(new P(\"alpha\", 5));\n"
        "        Console.WriteLine(r.Stock);\n"
        "        Console.WriteLine(r.Sku);\n"
        "    end Main;\n"
        "end Program;\n",
        "44\nalpha\nexit=0");
}

// CONTROL: a source that is NOT narrowed — a plain non-nullable `P` — is
// unaffected: the fix keys on the source's DECLARED nullable type and on the
// lowered value's carrier layout, so a bare record (whose own layout may
// coincidentally be `{i8, T}`) is never unwrapped.
BEZEL_TEST(codegen_with_type_narrowed_source_spec_12_7_9_5_7044,
           plain_non_nullable_source_unaffected) {
    expectOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var p: P := new P(\"a\", 5);\n"
        "        var r: P := p with(Stock := 44);\n"
        "        Console.WriteLine(r.Stock);\n"
        "    end Main;\n"
        "end Program;\n",
        "44\nexit=0");
}

// CONTROL: an UN-narrowed `P?` source at a `P` target is still rejected —
// the fix is a codegen representation fix, it does not relax §7.4's
// nullability rule at the initializer.
BEZEL_TEST(codegen_with_type_narrowed_source_spec_12_7_9_5_7044,
           unnarrowed_nullable_source_still_rejected) {
    auto r = runAot(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        var p := d.GetOrNull(\"a\");\n"
        "        var r: P := p with(Stock := 1);\n"
        "    end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(!r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is nullable but the target type is non-nullable") !=
            std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}
