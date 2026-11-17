//! case: s14_2_4931
//! doc: spec
//! section: 14.2
//! title: a choice that lists no IEquatable does not satisfy where T: IEquatable<T>
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the
//!   rule above.
//! cites: 14.2, 2.8
//! expect: reject
//! diagnostic: IEquatable
//! issue: 8578
module Conf;

choice Shape
  Sq(side: int),
  Pt;
end Shape;

public class Program
  public static method G<T>(x: T): int where T: IEquatable<T>
    return 0;
  end G;

  public static method Main()
    Console.WriteLine(G(Shape.Sq(1)));
  end Main;
end Program;
