//! case: l5_0039
//! doc: stdlib
//! section: 5
//! title: ElementAt with two arguments on an IReadOnlyList is rejected
//! rule: ElementAt(index: int) takes exactly one argument (stdlib 5).
//! cites: 5
//! expect: reject
//! diagnostic: argument
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    Console.WriteLine(r.ElementAt(1, 2));
  end Main;
end Program;
