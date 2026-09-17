//! case: s3_1_0001
//! doc: spec
//! section: 3.1
//! title: an interface variable may hold a boxed record value
//! rule: An interface variable is a reference; its underlying value may be a
//!   class instance, a boxed record, a boxed enum, or a boxed primitive.
//! cites: 3.1, 3.2, 8.6
//! expect: run
//! stdout: circle 5
module Conf;

public interface IShape
  method Describe(): string;
end IShape;

public record Circle(Radius: int): IShape
  public method Describe(): string => $"circle {Radius}";
end Circle;

public class Program
  public static method Main()
    var s: IShape := new Circle(5);
    Console.WriteLine(s.Describe());
  end Main;
end Program;
