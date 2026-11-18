//! case: s15_4_6701
//! doc: spec
//! section: 15.4
//! title: a parenthesised collection literal at Concat's IEnumerable parameter has no target type
//! rule: The target types that admit collection literals are:
//! cites: 5.1
//! expect: reject
//! diagnostic: target
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1];
    var q := a.Concat(([2, 3]));
    Console.WriteLine(q.Count());
  end Main;
end Program;
