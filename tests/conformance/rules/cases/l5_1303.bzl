//! case: l5_1303
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull over a record element type returns the record or null
//! rule: null when index is out of range
//! cites: 15.1
//! expect: run
//! output: 6
//!   null
module Conf;

record P(X: int, Y: int);

public class Program
  public static method Main()
    using var ps := new Array<P>();
    ps.Add(new P(1, 2));
    ps.Add(new P(3, 4));
    ps.Add(new P(5, 6));
    var a := ps.Where(lambda(p) => p.X > 1).ElementAtOrNull(1);
    if a <> null then
      Console.WriteLine(a!.Y);
    else
      Console.WriteLine("null");
    end if;
    var b := ps.Select(lambda(p) => p).ElementAtOrNull(9);
    if b <> null then
      Console.WriteLine(b!.Y);
    else
      Console.WriteLine("null");
    end if;
  end Main;
end Program;
