//! case: s4_6_1205
//! doc: spec
//! section: 4.6
//! title:a string plus a string-valued call on an int in a typed lambda concatenates
//! rule:`s + t` concatenates two strings.
//! expect: run
//! stdout: a3
module Conf;

public class Program
  public static method Main()
    var f := lambda(x: int): string => "a" + x.ToString();
    Console.WriteLine(f(3));
  end Main;
end Program;
