//! case: s15_4_6702
//! doc: spec
//! section: 15.4
//! title: a collection literal at Queue Union's IEnumerable parameter has no target type
//! rule: The target types that admit collection literals are:
//! cites: 5.1
//! expect: reject
//! diagnostic: target
module Conf;

public class Program
  public static method Main()
    using var a: Queue<int> := [1];
    var q := a.Union([2, 3]);
    Console.WriteLine(q.Count());
  end Main;
end Program;
