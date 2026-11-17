//! case: s14_2_4928
//! doc: spec
//! section: 14.2
//! title: bool does not satisfy where T: IComparable<T>: Boolean declares only IEquatable<bool>
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the
//!   rule above.
//! cites: 14.2, 2.8
//! expect: reject
//! diagnostic: IComparable
//! issue: 8578
module Conf;

public class Program
  public static method F<T>(x: T): int where T: IComparable<T>
    return 0;
  end F;

  public static method Main()
    Console.WriteLine(F(true));
  end Main;
end Program;
