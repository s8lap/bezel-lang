// fix #7041 (§9.5, §7.4, v0.340): a `with` over a source that §7.4 has
// narrowed non-null is typed as the non-null record, not `P?`.
//
//   §9.5 (v0.340): the source expression must be record-typed; `with`
//   produces a copy with the named fields replaced.
//   §7.4 (v0.340): a null test whose branch exits narrows a nullable local; a
//   narrowed read is the non-null type.
//
// Before the fix `var r2: P := p with(...)` and `d[k] := p with(...)` were
// rejected as nullable-into-non-nullable. The sema change alone also needs the
// codegen pair: lowerWith re-wrapped the copy into a `{i8, P}` box.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string runOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        std::string s = "DID NOT RUN:";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error) s += " | " + d.message;
        return s;
    }
    return r.stdoutText + "exit=" + std::to_string(r.exitCode);
}

}  // namespace

// The issue's literal repro. Expected: prints 4.
BEZEL_TEST(codegen_with_narrowed_source_7041, issue_repro_verbatim) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        d[\"a\"] := new P(\"a\", 5);\n"
        "        var p := d.GetOrNull(\"a\");\n"
        "        if p = null then\n"
        "            return;\n"
        "        end;\n"
        "        var q: P := p;\n"
        "        var r2: P := p with(Stock := p.Stock - 1);\n"
        "        d[p.Sku] := p with(Stock := p.Stock - 1);\n"
        "        Console.WriteLine(d[\"a\"].Stock);\n"
        "    end Main;\n"
        "end Program;\n"),
        std::string("4\nexit=0"));
}

// Narrowed parameter and narrowed readonly field, string-field record, looped.
BEZEL_TEST(codegen_with_narrowed_source_7041, narrowed_parameter_and_field_looped) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Holder\n"
        "    public readonly Item: P?;\n"
        "    public constructor(i: P?)\n"
        "        Item := i;\n"
        "    end;\n"
        "    public method Dec(): P\n"
        "        if Item = null then\n"
        "            return new P(\"none\", 0);\n"
        "        end;\n"
        "        return Item with(Stock := Item.Stock - 1);\n"
        "    end Dec;\n"
        "end Holder;\n"
        "public class Program\n"
        "    public static method Bump(p: P?): P\n"
        "        if p = null then\n"
        "            return new P(\"z\", 0);\n"
        "        end;\n"
        "        var r: P := p with(Stock := p.Stock + 10, Sku := \"bang\");\n"
        "        return r;\n"
        "    end Bump;\n"
        "    public static method Main()\n"
        "        var t := 0;\n"
        "        for var i in 0..<100 do\n"
        "            var h := new Holder(new P(\"abc\", 5));\n"
        "            var a := h.Dec();\n"
        "            var b := Bump(new P(\"q\", 1));\n"
        "            var c := Bump(null);\n"
        "            t := t + a.Stock + b.Stock + c.Stock + b.Sku.Length;\n"
        "        end;\n"
        "        Console.WriteLine(t);\n"
        "    end Main;\n"
        "end Program;\n"),
        std::string("1900\nexit=0"));
}

// Control: an UN-narrowed `p with(...)` over `P?` stays `P?` and is still
// rejected at a `P` target.
BEZEL_TEST(codegen_with_narrowed_source_7041, unnarrowed_source_still_rejected) {
    auto r = runAot(
        "module M;\n"
        "public record P(Sku: string, Stock: int);\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d: Dictionary<string, P> := new Dictionary<string, P>();\n"
        "        var p := d.GetOrNull(\"a\");\n"
        "        var r2: P := p with(Stock := 1);\n"
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
