//! case: l2_9_0002
//! doc: stdlib
//! section: 2.9
//! title: a record listing IEquatable<T> satisfies it with no written Equals
//! rule: Records implement this implicitly -- once a record lists IEquatable<T>
//!   in its inheritance clause, the auto-generated Equals(T) counts as declaring
//!   the matching member, with no explicit body needed.
//! cites: 14.2
//! expect: run
//! stdout: True
module Conf;

public record Point(X: int, Y: int): IEquatable<Point>;

public static class Helpers
  public static method AreEqual<T>(a: T, b: T): bool where T: IEquatable<T> => a.Equals(b);
end Helpers;

public class Program
  public static method Main()
    Console.WriteLine(Helpers.AreEqual(new Point(1, 2), new Point(1, 2)));
  end Main;
end Program;
