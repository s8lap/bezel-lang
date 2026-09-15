//! case: l2_9_0001
//! doc: stdlib
//! section: 2.9
//! title: records implement IEquatable<T> implicitly
//! rule: Records implement IEquatable<T> implicitly -- a record satisfies a
//!   where T: IEquatable<T> constraint without declaring the interface.
//! expect: run
//! stdout: True
module Conf;

public record Point(X: int, Y: int);

public static class Helpers
  public static method AreEqual<T>(a: T, b: T): bool where T: IEquatable<T> => a.Equals(b);
end Helpers;

public class Program
  public static method Main()
    Console.WriteLine(Helpers.AreEqual(new Point(1, 2), new Point(1, 2)));
  end Main;
end Program;
