//! case: s7_5_0902
//! doc: spec
//! section: 7.5
//! title: ?.Reversed() over a non-null array receiver yields the reversed copy through ??
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.7, 4.4
//! expect: run
//! output: 300
//! issue: 8153
module Conf;

public class Program
  public static method Main()
    var ys: Array<string> := ["bbb", "a"];
    var fb: Array<string> := ["c", "d"];
    var xs: Array<string>? := ys;
    var k := 0;
    var i := 0;
    while i < 100 do
      var a := xs?.Reversed() ?? fb.Reversed();
      k := k + a.Count + a[0].Length;
      i := i + 1;
    end while;
    Console.WriteLine(k);
  end Main;
end Program;
