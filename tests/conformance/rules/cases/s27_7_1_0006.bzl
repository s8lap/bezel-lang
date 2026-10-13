//! case: s27_7_1_0006
//! doc: spec
//! section: 27.7.1
//! title: equal method references hash alike through Object under where T hashable
//! rule: a method reference's hash is derived from the method it denotes and,
//!   where the equality compares a receiver, from that receiver's identity
//!   hash, so any two references the equality makes equal hash alike ... and
//!   `where T: hashable` at `Object` promises nothing a function value breaks
//! cites: 27.7.1, 8.3, 14.2
//! expect: run
//! stdout: True
//! issue: 5905
module Conf;

public class Demo
  public static method Twice(x: int): int => x * 2;

  public static method Agree<T>(a: T, b: T): bool where T: hashable, notnull =>
    a = b and a.GetHashCode() = b.GetHashCode();

  public static method Main()
    var fn1: (int) -> int := Demo.Twice;
    var fn2: (int) -> int := Demo.Twice;
    var f: Object := fn1;
    var g: Object := fn2;
    Console.WriteLine(Demo.Agree<Object>(f, g));
  end Main;
end Demo;
