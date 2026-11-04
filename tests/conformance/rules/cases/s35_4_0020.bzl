//! case: s35_4_0020
//! doc: spec
//! section: 35.4
//! title: the same generic choice instantiated at a class is borrow-bearing, so `=` is not available
//! rule: Otherwise, a choice is **borrow-bearing** if it holds borrows.
//! cites: 35.4, 35.7
//! expect: reject
//! diagnostic: not auto-hashable
module Conf;

public class K end K;

public choice C<DateTime> At(p: DateTime), Never; end C;

public class Program
  public static method Main()
    using var k := new K();
    var x := C<K>.At(k);
    var y := x;
    Console.WriteLine(x = y);
  end Main;
end Program;
