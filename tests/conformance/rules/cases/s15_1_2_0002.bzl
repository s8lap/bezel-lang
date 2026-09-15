//! case: s15_1_2_0002
//! doc: spec
//! section: 15.1.2
//! title: an inline array cannot be passed where IEnumerable<T> is expected
//! rule: Passing an inline array to a method expecting `IEnumerable<T>` is
//!   rejected: inline arrays are not class types and declare no interface
//!   conformances (§15.1.2).
//! cites: 15.1.2
//! expect: reject
//! diagnostic: IEnumerable
module Conf;

public static class Util
  public static method Sum(items: IEnumerable<int>): int => 0;
end Util;

public class Program
  public static method Main()
    var arr: [3]int := [1, 2, 3];
    _ := Util.Sum(arr);
  end Main;
end Program;
