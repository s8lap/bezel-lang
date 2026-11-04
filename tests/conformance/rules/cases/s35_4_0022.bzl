//! case: s35_4_0022
//! doc: spec
//! section: 35.4
//! title: a choice mixing a DateTime payload with a class payload is borrow-bearing
//! rule: Otherwise, a choice is **borrow-bearing** if it holds borrows.
//! cites: 35.4, 35.7
//! expect: reject
//! diagnostic: not auto-hashable
module Conf;

public class K end K;

public choice Ev At(a: DateTime, b: K), Never; end Ev;

public class Program
  public static method Main()
    using var k := new K();
    var x := Ev.At(new DateTime(2020, 1, 2, 3, 4, 5), k);
    var y := x;
    Console.WriteLine(x = y);
  end Main;
end Program;
