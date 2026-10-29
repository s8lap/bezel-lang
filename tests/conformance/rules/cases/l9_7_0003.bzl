//! case: l9_7_0003
//! doc: stdlib
//! section: 9.7
//! title: a null-conditional Contains on an Array reaches the declared Array.Contains, not a user extension
//! rule: `Contains`, `Count`, `Reverse`, and `ToArray` resolve to the instance members and the same-named
//! cites: 9.7, 7.5
//! expect: run
//! output: True
//!   False
module Conf;

extension Array<int>
  public method Contains(x: int): bool => false;
end;

public class Program
  public static method Has(a: Array<int>?): bool
    return a?.Contains(2) ?? false;
  end Has;

  public static method Main()
    var xs: Array<int> := [1, 2];
    Console.WriteLine(Has(xs));
    Console.WriteLine(Has(null));
  end Main;
end Program;
