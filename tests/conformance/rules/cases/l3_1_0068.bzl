//! case: l3_1_0068
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Replace accepts fresh string producers, empty new text and named arguments
//! rule: public method Replace(oldValue: string, newValue: string): StringBuilder; ... `Insert` and `Remove` raise `IndexOutOfRangeException` when the index or the `(startIndex, length)` window falls outside `0..Length`; `Replace` raises `ArgumentException` when `oldValue` is empty.
//! cites: 3.1
//! expect: run
//! stdout: 1690
module Conf;

public class Program
  public static method Mk(s: string): string
    return s + "";
  end Mk;

  public static method Main()
    var total := 0;
    for var i in 0..<150 do
      using var sb := new StringBuilder("hello world");
      sb.Replace("he" + "llo", "he" + "llo");
      sb.Replace(Mk("hello"), Mk("bye"));
      sb.Replace("o" + "", Mk(""));
      sb.Replace(Mk("w"), $"[{i}]");
      var p := "r";
      sb.Replace(p, p + p);
      sb.Replace(p + p, p);
      sb.Replace(p, "R");
      sb.Replace(oldValue := "y" + "e", newValue := "Y" + "E");
      total += sb.ToString().Length;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
