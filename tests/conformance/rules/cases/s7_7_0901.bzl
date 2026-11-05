//! case: s7_7_0901
//! doc: spec
//! section: 7.7
//! title: a null array receiver sends ?? to the right ToArray() arm
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.5, 4.4
//! expect: run
//! output: 500
//! issue: 8153
module Conf;

public class Program
  public static method Main()
    var fb: Array<string> := ["ccc", "dd", "e"];
    var zs: Array<string>? := null;
    var k := 0;
    var i := 0;
    while i < 100 do
      var a := zs?.ToArray() ?? fb.ToArray();
      k := k + a.Count + a[1].Length;
      i := i + 1;
    end while;
    Console.WriteLine(k);
  end Main;
end Program;
