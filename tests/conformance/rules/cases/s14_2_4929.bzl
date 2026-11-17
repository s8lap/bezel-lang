//! case: s14_2_4929
//! doc: spec
//! section: 14.2
//! title: bool satisfies where T: IEquatable<T>: Boolean lists IEquatable<bool>
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the
//!   rule above.
//! cites: 14.2, 2.8
//! expect: run
//! stdout: 0
//! issue: 8578
module Conf;

public class Program
  public static method F<T>(x: T): int where T: IEquatable<T>
    return 0;
  end F;

  public static method Main()
    Console.WriteLine(F(true));
  end Main;
end Program;
