//! case: s14_2_6431
//! doc: spec
//! section: 14.2
//! title: a class via an interface extending IComparable<int> satisfies a
//!   IComparable<int> bound
//! rule: that lists I (or any interface extending I) in its inheritance clause,
//!   that inherits from a class which conforms to I
//! cites: 14.2
//! expect: run
//! stdout: accepted
module Conf;
public class Program
  public static method Main(): int
    return F(new Impl());
  end Main;
  public static method F<T>(x: T): int where T: IComparable<int>
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
public interface IIntC: IComparable<int>
end IIntC;
public class Impl: IIntC
  public constructor() end constructor;
  public method CompareTo(other: int): int
    return 0;
  end CompareTo;
end Impl;
