//! case: s15_4_6704
//! doc: spec
//! section: 15.4
//! title: a collection literal at Concat's IEnumerable<T> parameter in a generic body has no target type
//! rule: The target types that admit collection literals are:
//! cites: 5.1
//! expect: reject
//! diagnostic: target
module Conf;

public class Program
  public static method F<T>(a: Array<T>, x: T): int
    var q := a.Concat([x]);
    return q.Count();
  end F;
  public static method Main()
    var a: Array<int> := [1];
    Console.WriteLine(Program.F<int>(a, 2));
  end Main;
end Program;
