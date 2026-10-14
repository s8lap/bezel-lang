//! case: s14_2_0081
//! doc: spec
//! section: 14.2
//! title: a one-argument OwningBuffer over a record with a non-defaultable enum field is rejected
//! rule: default(T) is a compile error for a record any of whose declared fields is not defaultable, including an enum with no zero-valued constant (17.11); the value presentation's one-argument constructor rejects the use.
//! cites: 14.2, 17.11
//! expect: reject
//! diagnostic: default
//! issue: 5821
module Conf;

public enum Level Low := 1, High := 2; end Level;

public record Pair(A: int, B: Level);

public class Program
  public static method Main()
    using var b := new OwningBuffer<Pair>(4);
    Console.WriteLine(b.Length);
  end Main;
end Program;
