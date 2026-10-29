//! case: l3_1_0067
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Insert accepts fresh string producers at the start, the end and mid-string, 150 loops
//! rule: public method Insert(index: int, value: string): StringBuilder; ... `Insert` and `Remove` raise `IndexOutOfRangeException` when the index or the `(startIndex, length)` window falls outside `0..Length`; `Replace` raises `ArgumentException` when `oldValue` is empty.
//! cites: 3.1
//! expect: run
//! stdout: 4418
module Conf;

public class Program
  public static method Mk(s: string): string
    return s + "!";
  end Mk;

  public static method Main()
    var total := 0;
    for var i in 0..<150 do
      using var sb := new StringBuilder();
      sb.Insert(0, "" + "");
      sb.Insert(0, Mk("a"));
      sb.Insert(sb.Length, Mk("b"));
      sb.Insert(0, Mk("c"));
      sb.Insert(1, "x" + i.ToString());
      sb.Insert(1, $"<{i}>");
      sb.Insert(2, Mk(""));
      sb.Insert(0, "lit");
      var p := "bor";
      sb.Insert(0, p);
      sb.Insert(0, p + p);
      sb.Insert(0, (if i > 5 then p else "q"));
      total += sb.ToString().Length;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
