//! case: s26_5_2410
//! doc: spec
//! section: 26.5
//! title: For int and string elements the stdlib Min()/Max() wins over a user
//!   extension on Object
//! rule: An extension on E1 is **more specific** than one on E2 when E1 is a
//!   subtype of E2 and E2 is not a subtype of E1.
//! cites: 26.5, 5.1
//! expect: run
//! output: 3
//!   8
//!   apple
//!   zebra
//! notes: Both candidates apply, because the element type is IComparable<its own
//!   type>: the stdlib extension on IEnumerable<T> and the user extension on
//!   Object. An interface converts directly to Object, so IEnumerable<T> is the
//!   subtype and its extension is more specific. Green on base as well as on
//!   the tip. int and string are IComparable of themselves (stdlib 9.1, 9.6);
//!   string order is ordinal.
module Conf;

extension Object
  public method Min(): int => 4242;
  public method Max(): int => 4343;
end;

public class Program
  public static method Main(): int
    using var a := new Array<int>();
    a.Add(8);
    a.Add(3);
    a.Add(6);
    Console.WriteLine(a.Min());
    Console.WriteLine(a.Max());
    using var b := new Array<string>();
    b.Add("pear");
    b.Add("apple");
    b.Add("zebra");
    Console.WriteLine(b.Min());
    Console.WriteLine(b.Max());
    return 0;
  end Main;
end Program;
