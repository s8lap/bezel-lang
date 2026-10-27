// fix #7088 (§12.7 / §7.4, v0.340): a §12.7-TYPE-NARROWED nullable RECORD
// carrier's MEMBER read addresses the carrier's slot as if it held the BARE
// record.
//
//   §12.7 (v0.340): "When the test succeeds and the tested expression is one
//   of §7.4's narrowing carriers, the carrier is **flow-narrowed** to `T`
//   within the controlled block" — `c is P` retypes the READ of `c` to `P`.
//   §7.4 (v0.340): the narrowing applies "inside the controlled block of a
//   successful `is` test".
//   §12.7 (v0.340): "**A narrowing converts nothing.** … retypes the carrier
//   and converts no value."
//   §7.2 (v0.340): a nullable value type is an optional — it either holds a
//   value or `null` — and its storage is the tagged {hasValue, payload} form.
//
// §12.7 retypes the READ, not the STORAGE: `c`'s slot is still the tagged
// `{i8, P}` nullable carrier, because the carrier's DECLARED type is `P?`.
// Codegen's `Emitter::fieldAddr` asked "is this a nullable record target?"
// from the target's SEMANTIC type — which the narrowing had already made
// non-nullable — so the payload GEP was skipped and the member path GEP'd the
// carrier's slot with the BARE record's element type.
//
// MEASURED on 3e3fc480 (the staging tip), the issue's literal repro:
//
//   %0 = alloca { i8, %record.P }, align 8      ; the carrier `c`
//   %23 = getelementptr inbounds nuw %record.P, ptr %0, i32 0, i32 0
//   %24 = load ptr, ptr %23, align 8
//
// `%record.P = { ptr, i32 }`, so field 0 (`Sku`) is at offset 0 — which is the
// carrier's OWN TAG BYTE, not the payload's first field — and field 1
// (`Stock`) is at offset 8, which is the `Sku` POINTER's low half. Hence
// `c.Stock` printed the pointer's low 32 bits (measured 3783648 and 2373392
// on two runs; 5 was expected) and `c.Sku` read the tag byte as a string
// pointer — the generated program died with a SIGSEGV inside
// `bz_string_to_utf8z`.
//
// §12.7 "a narrowing converts nothing" is the rule that settles the fix: the
// value in the slot is still the TAGGED carrier, so a member read of a
// narrowed carrier must take the carrier's PAYLOAD — the same unwrap #7044
// performs at the `with` site.
//
// The rows below assert the ANSWER (5 / a), not merely the absence of a
// crash, and run through runAotChecked so the generated code is
// ASan-instrumented: the pre-fix string-field read SEGFAULTs the generated
// program, so the row fails on the memory error as well as on the value.
// The CONTROL rows pin the shapes that already worked (the §7.4 null-narrowing
// form, the copy-out, the force-unwrap) and the shape that must NOT change
// (a record whose own layout coincides with a nullable carrier's).

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

// The §12.7-narrowed carrier's member read must not leave a sanitizer
// report behind: `runAotChecked` turns a crash or an ASan/LSan headline into
// an error diagnostic, so a clean row has none.
void expectNoSanitizerReport(const std::string& src) {
    auto r = runAotChecked(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        BEZEL_EXPECT_TRUE(d.message.find("sanitizer fired") ==
                          std::string::npos);
    }
}

}  // namespace

// The issue's literal repro (§12.7 narrowing + a member read of the
// narrowed carrier's int field). Expected: 5 — MEASURED 3783648 before the
// fix (the `Sku` POINTER's low 32 bits).
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           issue_repro_int_member_read_prints_5) {
    const std::string src =
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        d[\"a\"] := new P(\"a\", 5);\n"
        "        var c := d.GetOrNull(\"a\");\n"
        "        if c is P then\n"
        "            Console.WriteLine(c.Stock);\n"
        "        end;\n"
        "    end Main;\n"
        "end Program;\n";
    expectOut(src, "5\nexit=0");
    expectNoSanitizerReport(src);
}

// The same repro over the carrier's STRING field. Before the fix the read
// landed on the carrier's tag byte and the generated program SEGFAULTed
// inside `bz_string_to_utf8z` ("AddressSanitizer: SEGV on unknown address").
// Expected: the sku.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           issue_repro_string_member_read_prints_sku) {
    const std::string src =
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        d[\"a\"] := new P(\"a\", 5);\n"
        "        var c := d.GetOrNull(\"a\");\n"
        "        if c is P then\n"
        "            Console.WriteLine(c.Sku);\n"
        "        end;\n"
        "    end Main;\n"
        "end Program;\n";
    expectOut(src, "a\nexit=0");
    expectNoSanitizerReport(src);
}

// §12.7 narrows a PARAMETER carrier exactly as it narrows a local ("Narrowing
// applies to the original variable" — a parameter is one), so the member read
// of a narrowed parameter carrier must read the payload too.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           narrowed_parameter_carrier_member_read) {
    expectOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Show(p: P?)\n"
        "        if p is P then\n"
        "            Console.WriteLine(p.Stock);\n"
        "            Console.WriteLine(p.Sku);\n"
        "        end;\n"
        "    end Show;\n"
        "    public static method Main()\n"
        "        Show(new P(\"a\", 5));\n"
        "    end Main;\n"
        "end Program;\n",
        "5\na\nexit=0");
}

// A GENERIC record carrier narrows the same way, and the carrier's payload
// is the SPECIALIZED record struct (§14.1) — so the storage check must match
// the specialized carrier, not a template one.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           narrowed_generic_record_carrier_member_read) {
    expectOut(
        "module M;\n"
        "public record Box<T>(Value: T);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, Box<int>> := "
        "new Dictionary<string, Box<int>>();\n"
        "        d[\"a\"] := new Box<int>(5);\n"
        "        var c := d.GetOrNull(\"a\");\n"
        "        if c is Box<int> then\n"
        "            Console.WriteLine(c.Value);\n"
        "        end;\n"
        "    end Main;\n"
        "end Program;\n",
        "5\nexit=0");
}

// CONTROL: the §7.4 NULL-narrowing form (`if c <> null`) keeps the carrier's
// declared type `P?`, so the member path already read the payload. It must
// keep doing so — the fix must not disturb this path.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           null_narrowing_form_unaffected) {
    expectOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        d[\"a\"] := new P(\"a\", 5);\n"
        "        var c := d.GetOrNull(\"a\");\n"
        "        if c <> null then\n"
        "            Console.WriteLine(c.Stock);\n"
        "            Console.WriteLine(c.Sku);\n"
        "        end;\n"
        "    end Main;\n"
        "end Program;\n",
        "5\na\nexit=0");
}

// CONTROL: the copy-out (`var x: P := c;`) already unwrapped the carrier
// (§7.4's own copy path) and must keep doing so.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           copy_out_form_unaffected) {
    expectOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        d[\"a\"] := new P(\"a\", 5);\n"
        "        var c := d.GetOrNull(\"a\");\n"
        "        if c is P then\n"
        "            var x: P := c;\n"
        "            Console.WriteLine(x.Stock);\n"
        "            Console.WriteLine(x.Sku);\n"
        "        end;\n"
        "    end Main;\n"
        "end Program;\n",
        "5\na\nexit=0");
}

// CONTROL: the force-unwrap (`c!`) already extracts the payload and must
// keep doing so.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           force_unwrap_form_unaffected) {
    expectOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        d[\"a\"] := new P(\"a\", 5);\n"
        "        var c := d.GetOrNull(\"a\");\n"
        "        Console.WriteLine(c!.Stock);\n"
        "        Console.WriteLine(c!.Sku);\n"
        "    end Main;\n"
        "end Program;\n",
        "5\na\nexit=0");
}

// CONTROL: a record whose OWN layout coincides with a nullable carrier's —
// `Outer` is `{i8, Inner}`, byte-identical in shape to `Inner?`'s carrier.
// The member read of `o.Value` must read `Inner` itself, NOT the payload of
// an `Inner?` carrier: the fix keys on the STORAGE type (a bare record is an
// IDENTIFIED LLVM struct, a carrier is a LITERAL one, so the two can never
// be the same type), and this row proves it.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           coincident_layout_non_nullable_record_unaffected) {
    expectOut(
        "module M;\n"
        "public record Inner(Sku: string, Stock: int);\n"
        "public record Outer(Flag: byte, Value: Inner);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var o := new Outer(1, new Inner(\"a\", 5));\n"
        "        Console.WriteLine(o.Value.Stock);\n"
        "        Console.WriteLine(o.Value.Sku);\n"
        "    end Main;\n"
        "end Program;\n",
        "5\na\nexit=0");
}

// CONTROL: a plain non-nullable record source is unaffected — its member
// read addresses the record itself.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           plain_non_nullable_record_unaffected) {
    expectOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var p: P := new P(\"a\", 5);\n"
        "        Console.WriteLine(p.Stock);\n"
        "        Console.WriteLine(p.Sku);\n"
        "    end Main;\n"
        "end Program;\n",
        "5\na\nexit=0");
}

// CONTROL: #7044's site — the `with` copy over a §12.7-narrowed source must
// keep working (the sibling unwrap in `lowerWith` is a DIFFERENT site from
// the member path this issue fixes; neither may regress the other).
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           with_over_narrowed_source_unaffected) {
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
        "            Console.WriteLine(r.Sku);\n"
        "        end;\n"
        "    end Main;\n"
        "end Program;\n",
        "44\na\nexit=0");
}

// CONTROL: an UN-narrowed `P?` carrier is still rejected where a non-nullable
// record is required (§7.4) — the fix is a codegen representation fix, it
// relaxes no source rule.
BEZEL_TEST(codegen_narrowed_nullable_record_member_spec_12_7_7_4_7088,
           unnarrowed_nullable_member_access_still_rejected) {
    auto r = runAot(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        d[\"a\"] := new P(\"a\", 5);\n"
        "        var c := d.GetOrNull(\"a\");\n"
        "        Console.WriteLine(c.Stock);\n"
        "    end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(!r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("requires narrowing it to non-null first") !=
            std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}
