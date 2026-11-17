//! case: l4_4_2520
//! doc: stdlib
//! section: 4.4
//! title: Sorted() copies and Sort() sorts in place an Array of records keyed by a string literal
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 4.4, 2.8, 14.2
//! expect: run
//! output: lita:1
//!   litb:2
//!   litc:3
//!   lita:1
//!   litb:2
//!   litc:3
//!   2200
//! issue: 8584
module Conf;

public record S(name: string, n: int): IComparable<S>
  public method CompareTo(other: S): int
    return name.CompareTo(other.name);
  end CompareTo;
end S;

public class Program
  public static method Main()
    var tot := 0;
    for var i in 0..<200 do
      using var a: Array<S> := [new S("litc", 3), new S("lita", 1), new S("litb", 2)];
      using var s := a.Sorted();
      a.Sort();
      tot := tot + s[0].n * 10 + a[0].n;
      if i = 199 then
        for var e in s do
          Console.WriteLine($"{e.name}:{e.n}");
        end;
        for var e in a do
          Console.WriteLine($"{e.name}:{e.n}");
        end;
      end if;
    end for;
    Console.WriteLine(tot);
  end Main;
end Program;
