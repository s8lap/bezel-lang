//! case: s14_2_0079
//! doc: spec
//! section: 14.2
//! title: a one-argument OwningBuffer over an enum with no zero-valued constant is rejected
//! rule: The value presentation's one-argument constructor at a non-defaultable argument rejects the use: default(T) is a compile error for an enum type with no declared constant of underlying value 0 (17.11).
//! cites: 14.2, 17.11
//! expect: reject
//! diagnostic: default
//! issue: 5821
module Conf;

public enum Level Low := 1, High := 2; end Level;

public class Program
  public static method Main()
    using var b := new OwningBuffer<Level>(4);
    Console.WriteLine(b.Length);
  end Main;
end Program;
