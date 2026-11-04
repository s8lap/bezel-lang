//! case: l5_0201
//! doc: stdlib
//! section: 5
//! title: Min and Max with an int selector over a record that is not comparable are admitted
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5.1, 2.8
//! expect: run
//! output: 1
//!   30
//! issue: 8092
module Conf;

public record Pt(X: int, Y: int);

public class Program
  public static method Main()
    var a: Array<Pt> := [new Pt(3, 30), new Pt(1, 10), new Pt(2, 20)];
    Console.WriteLine(a.Min(lambda(p: Pt) => p.X));
    Console.WriteLine(a.Max(lambda(p: Pt) => p.Y));
  end Main;
end Program;
