//! case: s14_2_4933
//! doc: spec
//! section: 14.2
//! title: a TimeSpan does not satisfy where T: IEqualityComparer<T>: the stdlib record lists no such interface
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the
//!   rule above.
//! cites: 14.2, 2.8
//! expect: reject
//! diagnostic: IEqualityComparer
//! issue: 8578
module Conf;

public class Program
  public static method F<T>(x: T): int where T: IEqualityComparer<T>
    return 0;
  end F;

  public static method Main()
    Console.WriteLine(F(TimeSpan.FromTicks(5L)));
  end Main;
end Program;
