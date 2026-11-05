//! case: s7_7_0905
//! doc: spec
//! section: 7.7
//! title: a chain of ?? over a null-conditional Split and a final Split picks the first non-null operand
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.5, 9.6
//! expect: run
//! output: 300
//! issue: 8153
module Conf;

public class Program
  public static method Main()
    var t: string := "a,bc,dc";
    var s: string? := "a,bc,dc";
    var n: string? := null;
    var k := 0;
    var i := 0;
    while i < 100 do
      var a := n?.Split(",") ?? s?.Split(",") ?? t.Split(";");
      k := k + a.Count;
      i := i + 1;
    end while;
    Console.WriteLine(k);
  end Main;
end Program;
