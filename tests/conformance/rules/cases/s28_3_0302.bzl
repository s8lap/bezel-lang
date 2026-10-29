//! case: s28_3_0302
//! doc: spec
//! section: 28.3
//! title: a fresh owned StringBuilder in a short-circuited or conditional operand is produced only when run
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed ... `and` and `or` short-circuit: the right operand is not evaluated if the result is determined by the left.
//! cites: 28.3, 17.2
//! expect: run
//! output: 603
//!   533
module Conf;

public class Program
  public static method Fresh(sb: StringBuilder): owned StringBuilder
    sb.Append("R");
    return new StringBuilder("abc");
  end Fresh;

  public static method Main()
    var total := 0;
    using var sb := new StringBuilder();
    for var i in 0..<200 do
      if i mod 2 = 0 and Fresh(sb).Length > 0 then
        total += 1;
      end if;
      if i mod 3 = 0 or Fresh(sb).Insert(0, "z").ToString().Length > 100 then
        total += 2;
      end if;
      total += (if i mod 2 = 0 then Fresh(sb).Length else 0);
      while Fresh(sb).Length < 0 do
        total += 1000;
      end while;
      total += (if i mod 5 = 0 then new StringBuilder("q").Length else 0);
      if i mod 7 = 0 and new StringBuilder("w").Length > 0 then
        total += 1;
      end if;
    end for;
    Console.WriteLine(total);
    Console.WriteLine(sb.Length);
  end Main;
end Program;
