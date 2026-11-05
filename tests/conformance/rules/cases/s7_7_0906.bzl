//! case: s7_7_0906
//! doc: spec
//! section: 7.7
//! title: a for loop whose collection is a parenthesized ?? expression iterates the selected array
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.5, 9.6
//! expect: run
//! output: 500
//! issue: 8153
module Conf;

public class Program
  public static method Main()
    var t: string := "a,bc,dc";
    var s: string? := "a,bc,dc";
    var k := 0;
    var i := 0;
    while i < 100 do
      for var x in (s?.Split(",") ?? t.Split(";")) do
        k := k + x.Length;
      end for;
      i := i + 1;
    end while;
    Console.WriteLine(k);
  end Main;
end Program;
