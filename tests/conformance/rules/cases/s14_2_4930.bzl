//! case: s14_2_4930
//! doc: spec
//! section: 14.2
//! title: an enum that lists no IComparable does not satisfy where T: IComparable<T>
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the
//!   rule above.
//! cites: 14.2, 2.8
//! expect: reject
//! diagnostic: IComparable
//! issue: 8578
module Conf;

enum Color
  Red,
  Green;
end Color;

public class Program
  public static method F<T>(x: T): int where T: IComparable<T>
    return 0;
  end F;

  public static method Main()
    Console.WriteLine(F(Color.Red));
  end Main;
end Program;
