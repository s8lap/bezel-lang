//! case: s27_8_1404
//! doc: spec
//! section: 27.8
//! title: a generic alias keeps its body's Guid: a Dictionary<int, int> argument is rejected
//! rule: Each instantiation of the alias is interchangeable with the corresponding instantiation of
//!   the aliased type.
//! cites: 27.8, 5.1
//! expect: reject
//! diagnostic: implicitly convertible
module Conf;
type DG<T> := Dictionary<T, Guid>;
public class U
  public static method F<Guid>(d: DG<Guid>): int
    return d.Count;
  end F;
end U;
public class Program
  public static method Main()
    var d: Dictionary<int, int> := [1: 2];
    Console.WriteLine(U.F<int>(d));
  end Main;
end Program;
