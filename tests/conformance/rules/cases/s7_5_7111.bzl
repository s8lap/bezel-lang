//! case: s7_5_7111
//! doc: spec
//! section: 7.5
//! title: `?.FirstKey` / `?.LastKey` on a nullable SortedDictionary in locals, `??`, interpolation and calls
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7, 18.5
//! expect: run
//! stdout: 10000
module Conf;
public class Program
  public static method Take(s: string): int
    return s.Length;
  end Take;
  public static method Round(d: SortedDictionary<string, int>?): int
    var total := 0;
    var n := "n";
    var a := d?.FirstKey;
    if a <> null then total := total + a.Length; end;
    total := total + (d?.LastKey ?? "none").Length;
    var t := $"k={d?.FirstKey}|{d?.LastKey ?? n}";
    total := total + t.Length;
    var c := (d?.FirstKey ?? "q") + "!";
    total := total + c.Length;
    total := total + Take(d?.LastKey ?? "zz");
    return total;
  end Round;
  public static method Main()
    using var d := new SortedDictionary<string, int>();
    d.Add("pear" + "x", 1);
    d.Add("apple" + "y", 2);
    d.Add("zed" + "z", 3);
    var sum := 0;
    for var i in 0..<200 do
      sum := sum + Round(d);
      sum := sum + Round(null);
    end;
    Console.WriteLine(sum);
  end Main;
end Program;
