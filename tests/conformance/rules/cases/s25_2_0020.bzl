//! case: s25_2_0020
//! doc: spec
//! section: 25.2
//! title: a left operand converts to the first, interface-typed operator parameter
//! rule: "Parameters match operands in order" ([§25.2]). "classes by declaration or inheritance"
//!   ([§5.1]): the `Array<int>` left operand converts to the first parameter, `IEnumerable<int>`.
//! cites: 25.2, 5.1
//! expect: run
//! stdout: 1001
//! issue: 6217
module Conf;

public record V(X: int)
  public static method operator +(p: IEnumerable<int>, v: V): V => new V(v.X + 1000);
end V;

public class Program
  public static method Main()
    var v := new V(1);
    var a: Array<int> := [1, 2];
    Console.WriteLine((a + v).X);
  end Main;
end Program;
