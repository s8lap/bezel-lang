//! case: l2_9_0003
//! doc: stdlib
//! section: 2.9
//! title: a class implementing Equals(T) satisfies IEquatable<T>
//! rule: classes may implement IEquatable<T> for type-safe equality without
//!   boxing, by supplying Equals(other: T): bool.
//! expect: run
//! stdout: True
module Conf;

public class Point: IEquatable<Point>
  public readonly property X: int;
  public readonly property Y: int;

  public constructor(x: int, y: int)
    X := x;
    Y := y;
  end constructor;

  public method Equals(other: Point): bool => X = other.X and Y = other.Y;
end Point;

public static class Helpers
  public static method AreEqual<T>(a: T, b: T): bool where T: IEquatable<T> => a.Equals(b);
end Helpers;

public class Program
  public static method Main()
    Console.WriteLine(Helpers.AreEqual(new Point(1, 2), new Point(1, 2)));
  end Main;
end Program;
