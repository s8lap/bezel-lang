// Codegen — #7042 (per §22.10 / §15.4, v0.340): a collection literal passed
// DIRECTLY as a primary-constructor argument arrived EMPTY. `lowerExpr`
// returns null for a `CollectionLiteralExpr`, and the primary-constructor
// positional store loop in `lowerNew` did `if (!v) continue`, leaving the
// field calloc-zero. Silent wrong value (Count 0, exit 0, ASan quiet).
//
// Each row prints the Count, so a zero-filled field cannot pass. runAotChecked
// runs generated-code ASan WITH LeakSanitizer: the loop rows (200 iterations)
// pin that the fix neither leaks nor double-frees the literal.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string out(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText + "|exit=" + std::to_string(r.exitCode);
}
}  // namespace

// The issue's literal repro (§22.10 field initializer + member read; §15.4).
BEZEL_TEST(primary_ctor_collection_literal_arg_7042, issue_repro_array_literal_counts) {
    std::string src =
        "module M;\n"
        "public class A(xs: Array<int>)\n"
        "    private _n: int := xs.Count;\n"
        "    public method N(): int => _n;\n"
        "    public method Live(): int => xs.Count;\n"
        "end A;\n"
        "public class B\n"
        "    private _n: int;\n"
        "    public constructor(xs: Array<int>)\n"
        "        _n := xs.Count;\n"
        "    end;\n"
        "    public method N(): int => _n;\n"
        "end B;\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        Console.WriteLine($\"{new A([1, 2, 3]).N()}\");\n"
        "        Console.WriteLine($\"{new A([1, 2, 3]).Live()}\");\n"
        "        Console.WriteLine($\"{new B([1, 2, 3]).N()}\");\n"
        "    end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(src), "3\n3\n3\n|exit=0");
}

// Set<int> literal (§15.4: a Set target) and `owned` parameter.
BEZEL_TEST(primary_ctor_collection_literal_arg_7042, set_and_owned_param_literal_counts) {
    std::string src =
        "module M;\n"
        "public class S(s: Set<int>)\n"
        "    public method Live(): int => s.Count;\n"
        "end S;\n"
        "public class O(owned xs: Array<int>)\n"
        "    public method Live(): int => xs.Count;\n"
        "end O;\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        Console.WriteLine($\"{new S([4, 5]).Live()}\");\n"
        "        Console.WriteLine($\"{new O([1, 2, 3]).Live()}\");\n"
        "    end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(src), "2\n3\n|exit=0");
}

// Record and string elements, plain and owned parameters, looped: no leak,
// no double free, correct counts.
BEZEL_TEST(primary_ctor_collection_literal_arg_7042, record_and_string_elements_looped_no_leak) {
    std::string src =
        "module M;\n"
        "public record Product(Name: string, Qty: int);\n"
        "public class P(ps: Array<Product>)\n"
        "    public method Live(): int => ps.Count;\n"
        "end P;\n"
        "public class T(ts: Array<string>)\n"
        "    public method Live(): int => ts.Count + ts[0].Length;\n"
        "end T;\n"
        "public class OP(owned ps: Array<Product>)\n"
        "    public method Live(): int => ps.Count;\n"
        "end OP;\n"
        "public class OT(owned ts: Array<string>)\n"
        "    public method Live(): int => ts.Count + ts[0].Length;\n"
        "end OT;\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var total := 0;\n"
        "        for var i in 0..<200 do\n"
        "            var p := new P([new Product(\"x\", 1), new Product(\"y\", 2)]);\n"
        "            var t := new T([\"ab\", \"cde\"]);\n"
        "            var op := new OP([new Product(\"x\", 1), new Product(\"y\", 2)]);\n"
        "            var ot := new OT([\"ab\", \"cde\"]);\n"
        "            total += p.Live() + t.Live() + op.Live() + ot.Live();\n"
        "        end for;\n"
        "        Console.WriteLine($\"total {total}\");\n"
        "    end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(src), "total 2400\n|exit=0");
}
