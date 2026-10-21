//! case: s25_2_0016
//! doc: spec
//! section: 25.2
//! title: a binary operator's interface parameter accepts an array of the matching element type
//! rule: "For `a <op> b` on non-primitive operands, the candidate set is the union of the `operator
//!   <op>` overloads declared on the static types of both operands, including declarations
//!   inherited from their base classes" ([§25.2]). "classes by declaration or inheritance"
//!   ([§5.1]): `Array<int>` converts to `IEnumerable<int>`.
//! cites: 25.2, 5.1
//! expect: run
//! stdout: 1000
//! issue: 6217
module Conf;

public record V(X: int)
  public static method operator +(v: V, p: IEnumerable<int>): int => 1000;
end V;

public class Program
  public static method Main()
    var v := new V(1);
    var a: Array<int> := [1, 2];
    Console.WriteLine(v + a);
  end Main;
end Program;
