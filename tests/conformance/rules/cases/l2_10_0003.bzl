//! case: l2_10_0003
//! doc: stdlib
//! section: 2.10
//! title: where T: IDisposable admits every class implicitly
//! rule: Every class implicitly implements IDisposable, so the constraint
//!   where T: IDisposable admits every class -- with no interface declared.
//! expect: run
//! stdout: True
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public static class Helpers
  public static method Accepts<T>(x: T): bool where T: IDisposable => true;
end Helpers;

public class Program
  public static method Main()
    Console.WriteLine(Helpers.Accepts(new Resource()));
  end Main;
end Program;
