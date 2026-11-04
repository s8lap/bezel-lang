//! case: s21_1_0803
//! doc: spec
//! section: 21.1
//! title: a local's own initializer does not see the local, so Math.Sqrt in it is the built-in
//! rule: The declaration's own initializer is not in that scope
//! cites: 21.1, 2.3
//! expect: run
//! stdout: 4
//! issue: 8019
//! notes: admitting twin of s21_1_0801/0802: the declared local is a double, not a shadowing object.
module Conf;

public class Program
  public static method Main()
    var Math := Math.Sqrt(16.0);
    Console.WriteLine(Math);
  end Main;
end Program;
