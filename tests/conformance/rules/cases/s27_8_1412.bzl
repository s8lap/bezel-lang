//! case: s27_8_1412
//! doc: spec
//! section: 27.8
//! title: Counts<int> is Dictionary<int, int>: a Dictionary<string, int> argument is rejected
//! rule: Each instantiation of the alias is interchangeable with the corresponding instantiation of
//!   the aliased type.
//! cites: 27.8, 5.1
//! expect: reject
//! diagnostic: implicitly convertible
module Conf;
type Counts<T> := Dictionary<T, int>;
public class U
  public static method CountA<T>(d: Counts<T>): int
    return d.Count;
  end CountA;
end U;
public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    Console.WriteLine(U.CountA<int>(d));
  end Main;
end Program;
