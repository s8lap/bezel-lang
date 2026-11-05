//! case: s7_7_0903
//! doc: spec
//! section: 7.7
//! title: ?? returns the left Split result when its receiver is non-null, even if the right is nullable
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.5, 9.6
//! expect: run
//! output: 300
//! issue: 8153
module Conf;

public class Program
  public static method Main()
    var s: string? := "a,bc,dc";
    var n: string? := null;
    var k := 0;
    var i := 0;
    while i < 100 do
      var a := s?.Split(",") ?? n?.Split(",");
      if a <> null then
        k := k + a.Count;
      end if;
      i := i + 1;
    end while;
    Console.WriteLine(k);
  end Main;
end Program;
