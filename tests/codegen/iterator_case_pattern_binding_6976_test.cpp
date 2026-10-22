// Spec-derived regression test for issue #6976.
//
// Spec §29.3 (v0.340): an iterator is a state machine whose locals survive a
// `yield return`; §19.3: a case-pattern binding (`var n`, `var s: T`, a
// destructured component, at any nesting depth) is a local of its arm. §30.2:
// the same frame machinery serves `async` bodies across an `await`.
//
// THE DEFECT. collectSuspendLocals reserved frame fields for locals and loop
// variables but not for pattern bindings, so a binding read after the first
// suspend in a block-form arm came from a fresh stack slot (garbage; the issue
// repro printed `7 2370129`, expected `7 8`).
//
// Oracle: `runAotChecked`, exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}
}  // namespace

BEZEL_TEST(codegen_iterator_case_pattern_binding_6976, issue_repro_var_binding_with_guard) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "public class Program\n"
            "  public static method G(k: int): IEnumerable<int>\n"
            "    case k of\n"
            "      var n when n > 5 => do\n"
            "        yield return n;\n"
            "        yield return n + 1;\n"
            "      end,\n"
            "      _ => yield return 9;\n"
            "    end;\n"
            "  end G;\n"
            "  public static method Main()\n"
            "    for var v in G(7) do Console.Write(v.ToString() + \" \"); end; Console.WriteLine();\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("7 8 \n"));
}
BEZEL_TEST(codegen_iterator_case_pattern_binding_6976, string_record_nested_bindings_across_yield) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "record Pair(A: Pt, B: string);\n"
            "public class Program\n"
            "  public static method G(o: Object): IEnumerable<string>\n"
            "    case o of\n"
            "      var s: string when s.Length > 1 => do\n"
            "        yield return s;\n"
            "        yield return s + \"!\";\n"
            "      end,\n"
            "      _ => yield return \"other\";\n"
            "    end;\n"
            "  end G;\n"
            "  public static method GP(p: Pt): IEnumerable<string>\n"
            "    case p of\n"
            "      Pt(x, y) when x > 0 => do\n"
            "        yield return x.ToString();\n"
            "        yield return y.ToString();\n"
            "        yield return (x + y).ToString();\n"
            "      end,\n"
            "      _ => yield return \"other\";\n"
            "    end;\n"
            "  end GP;\n"
            "  public static method GQ(q: Pair): IEnumerable<string>\n"
            "    case q of\n"
            "      Pair(Pt(a, var b), t) => do\n"
            "        yield return t;\n"
            "        yield return a.ToString();\n"
            "        yield return b.ToString();\n"
            "        yield return t;\n"
            "      end\n"
            "    end;\n"
            "  end GQ;\n"
            "  public static method H(k: int): IEnumerable<int>\n"
            "    case k of\n"
            "      var n => do\n"
            "        yield return n;\n"
            "        yield return n * 2;\n"
            "      end\n"
            "    end;\n"
            "  end H;\n"
            "  public static method Main()\n"
            "    for var v in G(\"hi\") do Console.Write(v + \" \"); end; Console.WriteLine();\n"
            "    for var v in G(\"x\") do Console.Write(v + \" \"); end; Console.WriteLine();\n"
            "    for var v in GP(new Pt(3, 4)) do Console.Write(v + \" \"); end; Console.WriteLine();\n"
            "    for var v in GP(new Pt(0, 4)) do Console.Write(v + \" \"); end; Console.WriteLine();\n"
            "    for var v in GQ(new Pair(new Pt(5, 6), \"q\")) do Console.Write(v + \" \"); end; Console.WriteLine();\n"
            "    for var v in H(21) do Console.Write(v.ToString() + \" \"); end; Console.WriteLine();\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("hi hi! \nother \n3 4 7 \nother \nq 5 6 q \n21 42 \n"));
}

BEZEL_TEST(codegen_iterator_case_pattern_binding_6976, async_bindings_across_await) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "public class Program\n"
            "  public static async method F(k: int, o: Object, p: Pt): Task<int>\n"
            "    var total := 0;\n"
            "    case k of\n"
            "      var n when n > 5 => do\n"
            "        await Task.Delay(2);\n"
            "        total := total + n;\n"
            "        await Task.Delay(2);\n"
            "        total := total + n + 1;\n"
            "      end,\n"
            "      _ => total := -1;\n"
            "    end;\n"
            "    case o of\n"
            "      var s: string when s.Length > 1 => do\n"
            "        await Task.Delay(2);\n"
            "        total := total + s.Length;\n"
            "        await Task.Delay(2);\n"
            "        total := total + s.Length;\n"
            "      end,\n"
            "      _ => total := total - 100;\n"
            "    end;\n"
            "    case p of\n"
            "      Pt(x, var y) => do\n"
            "        await Task.Delay(2);\n"
            "        total := total + x * 100;\n"
            "        await Task.Delay(2);\n"
            "        total := total + y * 1000;\n"
            "      end\n"
            "    end;\n"
            "    return total;\n"
            "  end F;\n"
            "  public static async method Main(): Task<int>\n"
            "    var r := await F(7, \"abc\", new Pt(2, 3));\n"
            "    Console.WriteLine(r);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("3221\n"));
}

BEZEL_TEST(codegen_iterator_case_pattern_binding_6976, abandoned_arm_with_using_sentinel_100x) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "class Box\n"
            "  public static live: int := 0;\n"
            "  public static dead: int := 0;\n"
            "  public Id: int := 0;\n"
            "  public constructor(id: int) Id := id; Box.live := Box.live + 1; end constructor;\n"
            "  destructor Box.dead := Box.dead + 1; end destructor;\n"
            "end Box;\n"
            "public class Program\n"
            "  public static method G(o: Object): IEnumerable<string>\n"
            "    case o of\n"
            "      var s: string when s.Length > 0 => do\n"
            "        using var b := new Box(s.Length);\n"
            "        yield return s + b.Id.ToString();\n"
            "        yield return s + \"!\" + b.Id.ToString();\n"
            "      end,\n"
            "      _ => yield return \"none\";\n"
            "    end;\n"
            "  end G;\n"
            "  public static method Main()\n"
            "    var tot := 0;\n"
            "    for var i in 0..<100 do\n"
            "      for var v in G(\"ab\") do\n"
            "        tot := tot + v.Length;\n"
            "        break;\n"
            "      end;\n"
            "      for var v in G(\"abc\") do\n"
            "        tot := tot + v.Length;\n"
            "      end;\n"
            "    end;\n"
            "    Console.WriteLine(tot.ToString() + \" \" + (Box.live - Box.dead).ToString() + \" \" + Box.live.ToString());\n"
            "    for var v in G(\"ab\") do Console.Write(v + \" \"); end; Console.WriteLine();\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("1200 0 200\nab2 ab!2 \n"));
}
