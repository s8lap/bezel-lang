//! case: s7_5_0901
//! doc: spec
//! section: 7.5
//! title: a non-null array receiver of ?.ToArray() yields the copy, and the ?? right arm is not taken
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.7, 4.4
//! expect: run
//! output: 400
//! issue: 8153
//! notes: Left arm of a ?? over two ToArray() materializers; the value pins which arm ran.
module Conf;

public class Program
  public static method Main()
    var ys: Array<string> := ["x", "yy"];
    var fb: Array<string> := ["ccc", "dd", "e"];
    var xs: Array<string>? := ys;
    var k := 0;
    var i := 0;
    while i < 100 do
      var a := xs?.ToArray() ?? fb.ToArray();
      k := k + a.Count + a[1].Length;
      i := i + 1;
    end while;
    Console.WriteLine(k);
  end Main;
end Program;
