//! case: s25_2_0021
//! doc: spec
//! section: 25.2
//! title: a left operand that does not implement the first parameter's interface is rejected
//! rule: "Parameters match operands in order" ([§25.2]). "nor an overload from the candidate set above
//!   applies is a compile error at the expression" ([§25.2]): a `StringBuilder` left operand
//!   converts to no first parameter.
//! cites: 25.2, 5.1
//! expect: reject
//! diagnostic: is not defined for type
//! issue: 6217
module Conf;

public record V(X: int)
  public static method operator +(p: IEnumerable<int>, v: V): V => new V(v.X + 1000);
end V;

public class Program
  public static method Main()
    var v := new V(1);
    using var a := new StringBuilder();
    Console.WriteLine((a + v).X);
  end Main;
end Program;
