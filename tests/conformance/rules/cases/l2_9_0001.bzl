//! case: l2_9_0001
//! doc: stdlib
//! section: 2.9
//! title: a record's auto-generated equality does not by itself satisfy IEquatable<T>
//! rule: A record conforms to an interface only when it lists that interface in
//!   its inheritance clause and declares matching members; a plain record with
//!   no such clause does not conform to IEquatable<T>, whatever equality
//!   operators it auto-generates.
//! cites: 14.2
//! expect: reject
//! diagnostic: IEquatable
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
