// Codegen — #7046 (per §22.10, §22.5, §15.4, v0.340): a collection literal
// reaching a primary-constructor parameter through (1) a DEFAULT value
// (`ys: Array<int> := [7, 8]`) or (2) a BASE-CLAUSE argument
// (`class D(d: Array<int>): Base([1, 2, 3, 4])`) arrived EMPTY. `lowerExpr`
// returns null for a `CollectionLiteralExpr`; the default-fill
// (`positionalParamDefaultValue`) and the base-clause / `base(...)` argument
// loops (`prePopulateBasePositionalFields`) left the slot calloc-zero. The
// positional argument of `new` (#7042) and the `this([..])` chain were already
// correct.
//
// Every row prints Counts, so a zero-filled slot cannot pass. runAotChecked
// runs generated-code ASan WITH LeakSanitizer; the loop rows pin that the fix
// neither leaks nor double-frees the literal.

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

// The issue's literal repro: default + base clause. (The repro's `this([..])`
// chain class `C` is omitted: its value is correct but it leaks under LSan, a
// separate defect filed apart from #7046.)
BEZEL_TEST(primary_ctor_literal_default_and_base_7046, issue_repro_default_and_base_clause) {
    std::string src =
        "module M;\n"
        "public class A(xs: Array<int>, ys: Array<int> := [7, 8])\n"
        "    public method Live(): int => xs.Count * 10 + ys.Count;\n"
        "end A;\n"
        "public class Base(bs: Array<int>)\n"
        "    public method BL(): int => bs.Count;\n"
        "end Base;\n"
        "public class D(d: Array<int>): Base([1, 2, 3, 4])\n"
        "    public method DL(): int => d.Count;\n"
        "end D;\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var a := new A([1, 2, 3]);\n"
        "        var d := new D([1]);\n"
        "        Console.WriteLine($\"{a.Live()} {d.BL()} {d.DL()}\");\n"
        "    end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(src), "32 4 1\n|exit=0");
}

// Default alone, and a default overridden by an explicit argument (§22.5: the
// default applies only on omission).
BEZEL_TEST(primary_ctor_literal_default_and_base_7046, default_literal_omitted_and_overridden) {
    std::string src =
        "module M;\n"
        "public class A(ys: Array<int> := [7, 8, 9])\n"
        "    public method N(): int => ys.Count;\n"
        "    public method Sum(): int => ys[0] + ys[1];\n"
        "end A;\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var a := new A();\n"
        "        var b := new A([1, 2]);\n"
        "        Console.WriteLine($\"{a.N()} {a.Sum()} {b.N()} {b.Sum()}\");\n"
        "    end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(src), "3 15 2 3\n|exit=0");
}

// Base clause alone, including the explicit `base([..])` chain of a derived
// class with an explicit constructor over a primary-constructor base.
BEZEL_TEST(primary_ctor_literal_default_and_base_7046, base_clause_and_base_chain_literal) {
    std::string src =
        "module M;\n"
        "public class Base(bs: Array<int>)\n"
        "    public method BL(): int => bs.Count;\n"
        "    public method BS(): int => bs[0] + bs[3];\n"
        "end Base;\n"
        "public class D(d: int): Base([1, 2, 3, 4])\n"
        "    public method DL(): int => d;\n"
        "end D;\n"
        "public class E: Base\n"
        "    public constructor(k: int)\n"
        "        base([10, 20, 30]);\n"
        "    end;\n"
        "end E;\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var d := new D(9);\n"
        "        var e := new E(1);\n"
        "        Console.WriteLine($\"{d.BL()} {d.BS()} {d.DL()} {e.BL()}\");\n"
        "    end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(src), "4 5 9 3\n|exit=0");
}

// Set and Dictionary literals (§15.4) in the default and base-clause
// positions, and a generic base (`Base<int>([..])`).
BEZEL_TEST(primary_ctor_literal_default_and_base_7046, set_dictionary_and_generic_base_literals) {
    std::string src =
        "module M;\n"
        "public class L(xs: Set<int> := [4, 5, 6], d: Dictionary<string, int> := [\"a\": 1, \"b\": 2])\n"
        "    public method Live(): int => xs.Count * 10 + d.Count;\n"
        "end L;\n"
        "public class GB<T>(bs: Array<T>)\n"
        "    public method BL(): int => bs.Count;\n"
        "end GB;\n"
        "public class GD(d: int): GB<int>([1, 2, 3, 4, 5])\n"
        "    public method DL(): int => d;\n"
        "end GD;\n"
        "public class SB(s: Set<int>)\n"
        "    public method N(): int => s.Count;\n"
        "end SB;\n"
        "public class SD(): SB([1, 2, 2, 3])\n"
        "end SD;\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var g := new GD(9);\n"
        "        Console.WriteLine($\"{new L().Live()} {g.BL()} {g.DL()} {new SD().N()}\");\n"
        "    end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(src), "32 5 9 3\n|exit=0");
}

// 200 iterations, string / record elements, plain and `owned` parameters, in
// the default and base-clause positions: correct totals, no leak, no double
// free (LeakSanitizer is armed).
BEZEL_TEST(primary_ctor_literal_default_and_base_7046, default_and_base_literal_looped_no_leak) {
    std::string src =
        "module M;\n"
        "public record Product(Name: string, Qty: int);\n"
        "public class P(n: int, ps: Array<Product> := [new Product(\"x\", 1), new Product(\"y\", 2)])\n"
        "    public method Live(): int => ps.Count + n;\n"
        "end P;\n"
        "public class T(ts: Array<string> := [\"ab\", \"cde\"])\n"
        "    public method Live(): int => ts.Count + ts[0].Length;\n"
        "end T;\n"
        "public class OT(owned ts: Array<string> := [\"ab\", \"cde\"])\n"
        "    public method Live(): int => ts.Count + ts[0].Length;\n"
        "end OT;\n"
        "public class Base(ts: Array<string>)\n"
        "    public method BL(): int => ts.Count + ts[1].Length;\n"
        "end Base;\n"
        "public class D(n: int): Base([\"ab\", \"cde\"])\n"
        "    public method DL(): int => n;\n"
        "end D;\n"
        "public class OBase(owned ts: Array<string>)\n"
        "    public method BL(): int => ts.Count + ts[1].Length;\n"
        "end OBase;\n"
        "public class OD(n: int): OBase([\"ab\", \"cde\"])\n"
        "    public method DL(): int => n;\n"
        "end OD;\n"
        "public class Program\n"
        "    public static method Main()\n"
        "        var total := 0;\n"
        "        for var i in 0..<200 do\n"
        "            var p := new P(1);\n"
        "            var t := new T();\n"
        "            var ot := new OT();\n"
        "            var d := new D(1);\n"
        "            var od := new OD(1);\n"
        "            total += p.Live() + t.Live() + ot.Live() + d.BL() + d.DL() + od.BL() + od.DL();\n"
        "        end for;\n"
        "        Console.WriteLine($\"total {total}\");\n"
        "    end Main;\n"
        "end Program;\n";
    // p=3, t=4, ot=4, d=5+1, od=5+1 -> 3+4+4+6+6 = 23 per iteration.
    BEZEL_EXPECT_EQ(out(src), "total 4600\n|exit=0");
}
