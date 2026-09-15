//! case: s26_5_1_0001
//! doc: spec
//! section: 26.5.1
//! title: an extension on a class outranks one on an interface it implements
//! rule: An extension on E1 is more specific than one on E2 when E1 is a
//!   subtype of E2 and E2 is not a subtype of E1. Class beats interface
//!   implemented by that class: for a receiver of type `Array<int>`,
//!   `extension Array<T>` is more specific than `extension IEnumerable<T>`.
//! cites: 26.5.1, 26.5
//! expect: run
//! stdout: 1
module Conf;

extension IEnumerable<T> where T: value
  public method First(): T
    for var item in this do return item; end;
    throw new InvalidOperationException("empty sequence");
  end First;
end;

extension Array<T>
  public method First(): T
    if this.Count = 0 then throw new InvalidOperationException("empty array"); end;
    return this[0];
  end First;
end;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(xs.First());
  end Main;
end Program;
