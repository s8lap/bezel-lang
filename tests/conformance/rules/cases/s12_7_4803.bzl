//! case: s12_7_4803
//! doc: spec
//! section: 12.7
//! title: is T? and as T? over a type parameter substituted by Guid, DateTime and int
//! rule: tests as its underlying type: `x is T?` has the answer of `x is T`
//! cites: 12.7, 3.6
//! expect: run
//! output: True
//!   False
//!   True
//!   True
//!   True
//!   False
//!   True
//!   True
//! issue: 8593
module Conf;

public static class Gen
  public static method Is<T>(o: Object): bool
    return o is T?;
  end Is;

  public static method As<T>(o: Object): bool
    var x: T? := o as T?;
    return x <> null;
  end As;
end Gen;

public class Program
  public static method Main()
    Console.WriteLine(Gen.Is<Guid>(Guid.Empty));
    Console.WriteLine(Gen.Is<Guid>(5));
    Console.WriteLine(Gen.Is<DateTime>(new DateTime(2020, 1, 1)));
    Console.WriteLine(Gen.Is<int>(5));
    Console.WriteLine(Gen.As<Guid>(Guid.Empty));
    Console.WriteLine(Gen.As<Guid>(7));
    Console.WriteLine(Gen.As<DateTime>(new DateTime(2020, 1, 1)));
    Console.WriteLine(Gen.As<int>(7));
  end Main;
end Program;
