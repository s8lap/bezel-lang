//! case: s14_2_0011
//! doc: spec
//! section: 14.2
//! title: a defaultable record satisfies where T: new() and new T() is default(T)
//! rule: A value-type substitution satisfies `where T: new()` exactly when it
//!   is defaultable; inside the generic body, `new T()` for a value
//!   substitution is equivalent to `default(T)` (§14.2).
//! cites: 14.2, 17.11
//! expect: run
//! stdout: 0
module Conf;

public record Point(X: int, Y: int);

public static class Factory
  public static method Make<T>(): owned T where T: new() => new T();
end Factory;

public class Program
  public static method Main()
    var p := Factory.Make<Point>();
    Console.WriteLine(p.X);
  end Main;
end Program;
