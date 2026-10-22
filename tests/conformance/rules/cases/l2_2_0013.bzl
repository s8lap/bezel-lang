//! case: l2_2_0013
//! doc: stdlib
//! section: 2.2
//! title: Sign inside a generic body compares in the instantiated unsigned or signed type
//! rule: public static method Sign<T>(x: T): int where T: INumber<T>; -1, 0, or 1:
//!   two `<` tests against ... that obligation is what entitles a `where T:
//!   INumber<T>` or `where T: ISteppable<T>` body to all four comparisons
//!   (§25.2, *Type-parameter operands*).
//! cites: 2.2, 2.19
//! expect: run
//! output: 1
//!   1
//!   -1
//! issue: 6925
module Conf;

public static class Util
  public static method S<T>(a: T): int where T: INumber<T>
    return Math.Sign(a);
  end S;
end Util;

public class Program
  public static method Main()
    Console.WriteLine(Util.S<uint>(4000000000u));
    Console.WriteLine(Util.S<ulong>(18000000000000000000ul));
    Console.WriteLine(Util.S<int>(-7));
  end Main;
end Program;
