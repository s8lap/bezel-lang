//! case: s17_5_0014
//! doc: spec
//! section: 17.5
//! title: a class left operand is admitted into an Array of an interface it implements
//! rule: "a type to an interface it implements or conforms to: classes by
//!   declaration or inheritance" is an implicit conversion ([§5.1]), so the
//!   left operand is admissible as `Contains(item: IShape)`'s argument
//!   ([§17.5]).
//! cites: 17.5, 5.1, 8.3
//! expect: run
//! stdout: True
module Conf;

public interface IShape
end IShape;

public class Square: IShape
end Square;

public class Program
  public static method Main()
    var s := new Square();
    var a: Array<IShape> := [s];
    var found := s in a;
    Console.WriteLine(found);
  end Main;
end Program;
