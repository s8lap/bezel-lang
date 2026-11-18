//! case: s15_4_6705
//! doc: spec
//! section: 15.4
//! title: a typed Array local at Concat's IEnumerable parameter is admitted
//! rule: It is well-typed only in a context that provides the target type: a **type annotation**, or the parameter type of a method call.
//! cites: 5.1
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1];
    var b: Array<int> := [2, 3];
    var q := a.Concat(b);
    Console.WriteLine(q.Count());
  end Main;
end Program;
