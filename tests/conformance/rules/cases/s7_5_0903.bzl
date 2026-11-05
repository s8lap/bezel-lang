//! case: s7_5_0903
//! doc: spec
//! section: 7.5
//! title: a ?. Split result passed as an argument is the array for a non-null receiver and null otherwise
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.7, 9.6
//! expect: run
//! output: 98
//!   -100
//! issue: 8153
module Conf;

public class Program
  public static method CountOf(a: Array<string>?): int
    if a = null then
      return -1;
    end if;
    return a.Count;
  end CountOf;

  public static method Main()
    var s: string? := "a,bc,dc";
    var n: string? := null;
    var k := 0;
    var m := 0;
    var i := 0;
    while i < 100 do
      if i > 1 and CountOf(s?.Split(",")) = 3 then
        k := k + 1;
      end if;
      m := m + CountOf(n?.Split(","));
      i := i + 1;
    end while;
    Console.WriteLine(k);
    Console.WriteLine(m);
  end Main;
end Program;
