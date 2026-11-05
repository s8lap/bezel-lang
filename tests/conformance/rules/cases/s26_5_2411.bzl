//! case: s26_5_2411
//! doc: spec
//! section: 26.5
//! title: For decimal elements the stdlib Min()/Max() wins over a user extension on
//!   Object
//! rule: An extension on E1 is **more specific** than one on E2 when E1 is a
//!   subtype of E2 and E2 is not a subtype of E1.
//! cites: 26.5, 5.1
//! expect: run
//! output: 2.25
//!   8.5
//! notes: Both candidates apply, because the element type is IComparable<its own
//!   type>: the stdlib extension on IEnumerable<T> and the user extension on
//!   Object. An interface converts directly to Object, so IEnumerable<T> is the
//!   subtype and its extension is more specific. Green on base as well as on
//!   the tip. Decimal is IComparable<decimal> (stdlib 9.3).
module Conf;

extension Object
  public method Min(): int => 4242;
  public method Max(): int => 4343;
end;

public class Program
  public static method Main(): int
    using var a := new Array<decimal>();
    a.Add(8.5m);
    a.Add(2.25m);
    a.Add(6m);
    Console.WriteLine(a.Min());
    Console.WriteLine(a.Max());
    return 0;
  end Main;
end Program;
