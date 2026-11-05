//! case: s7_7_0902
//! doc: spec
//! section: 7.7
//! title: ?? over two null-conditional Split results with both receivers null is null
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.5, 9.6
//! expect: run
//! output: -100
//! issue: 8153
module Conf;

public class Program
  public static method Main()
    var n: string? := null;
    var k := 0;
    var i := 0;
    while i < 100 do
      var a := n?.Split(",") ?? n?.Split(",");
      if a = null then
        k := k - 1;
      else
        k := k + a.Count;
      end if;
      i := i + 1;
    end while;
    Console.WriteLine(k);
  end Main;
end Program;
