//! case: l5_0037
//! doc: stdlib
//! section: 5
//! title: First with three arguments on an IReadOnlyList is rejected
//! rule: First is declared with no parameters and with one predicate parameter (stdlib 5); no overload takes three arguments.
//! cites: 5
//! expect: reject
//! diagnostic: argument
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    Console.WriteLine(r.First(1, 2, 3));
  end Main;
end Program;
