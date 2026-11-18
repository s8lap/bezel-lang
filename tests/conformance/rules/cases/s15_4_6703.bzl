//! case: s15_4_6703
//! doc: spec
//! section: 15.4
//! title: a collection literal at Zip's IEnumerable parameter after a lazy Select has no target type
//! rule: The target types that admit collection literals are:
//! cites: 5.1
//! expect: reject
//! diagnostic: target
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1];
    var q := a.Select(lambda(x: int) => x + 1).Zip([2, 3], lambda(x: int, y: int) => x + y);
    Console.WriteLine(q.Count());
  end Main;
end Program;
