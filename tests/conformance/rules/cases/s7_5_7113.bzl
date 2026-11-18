//! case: s7_5_7113
//! doc: spec
//! section: 7.5
//! title: `?.FirstKey` on a non-null empty SortedDictionary raises; a null receiver gives null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! stdout: -20000
module Conf;
public class Program
  public static method F(d: SortedDictionary<string, int>?): int
    try
      var k := d?.FirstKey;
      if k = null then return -1; end;
      return k.Length;
    catch InvalidOperationException =>
      return -99;
    end;
  end F;
  public static method Main()
    using var d := new SortedDictionary<string, int>();
    var sum := 0;
    for var i in 0..<200 do
      sum := sum + F(d);
      sum := sum + F(null);
    end;
    Console.WriteLine(sum);
  end Main;
end Program;
