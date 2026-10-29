//! case: l3_1_0069
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder Insert and Replace raise on a bad index or an empty old value from fresh producers
//! rule: `Insert` and `Remove` raise `IndexOutOfRangeException` when the index or the `(startIndex, length)` window falls outside `0..Length`; `Replace` raises `ArgumentException` when `oldValue` is empty.
//! cites: 3.1
//! expect: run
//! stdout: 600
module Conf;

public class Program
  public static method Bang(s: string): string
    return s + "!";
  end Bang;

  public static method Main()
    var caught := 0;
    for var i in 0..<150 do
      using var sb := new StringBuilder("abc");
      try
        sb.Insert(99, Bang("x"));
      catch IndexOutOfRangeException => caught += 1;
      end try;
      try
        sb.Insert(-1, "q" + "r");
      catch IndexOutOfRangeException => caught += 1;
      end try;
      try
        sb.Replace("" + "", "zz");
      catch ArgumentException => caught += 1;
      end try;
      try
        sb.Replace("", "zz" + "y");
      catch ArgumentException => caught += 1;
      end try;
    end for;
    Console.WriteLine(caught);
  end Main;
end Program;
