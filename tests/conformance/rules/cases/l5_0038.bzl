//! case: l5_0038
//! doc: stdlib
//! section: 5
//! title: Zip with one argument on an IReadOnlyList is rejected
//! rule: Zip<U, R>(other, combine) takes two arguments (stdlib 5); a call with one has no applicable overload.
//! cites: 5
//! expect: reject
//! diagnostic: argument
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    Console.WriteLine(l.Zip(r).Count());
  end Main;
end Program;
