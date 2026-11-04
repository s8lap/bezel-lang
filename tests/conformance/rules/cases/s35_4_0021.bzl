//! case: s35_4_0021
//! doc: spec
//! section: 35.4
//! title: a choice whose payload is a user class named Guid is borrow-bearing
//! rule: Otherwise, a choice is **borrow-bearing** if it holds borrows.
//! cites: 35.4, 35.7
//! expect: reject
//! diagnostic: not auto-hashable
module Conf;

public class Guid end Guid;

public choice Ev At(p: Guid), Never; end Ev;

public class Program
  public static method Main()
    using var g := new Guid();
    var x := Ev.At(g);
    var y := x;
    Console.WriteLine(x = y);
  end Main;
end Program;
