//! case: l2_9_0002
//! doc: stdlib
//! section: 2.9
//! title: a plain class does not implicitly satisfy IEquatable<T>
//! rule: Records implement IEquatable<T> implicitly; classes may implement it
//!   -- but do not implicitly -- for type-safe equality.
//! expect: reject
//! diagnostic: IEquatable
module Conf;

public class Point
  public readonly property X: int;
  public readonly property Y: int;

  public constructor(x: int, y: int)
    X := x;
    Y := y;
  end constructor;
end Point;

public static class Helpers
  public static method AreEqual<T>(a: T, b: T): bool where T: IEquatable<T> => a.Equals(b);
end Helpers;

public class Program
  public static method Main()
    Console.WriteLine(Helpers.AreEqual(new Point(1, 2), new Point(1, 2)));
  end Main;
end Program;
