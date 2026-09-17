//! case: s17_11_0015
//! doc: spec
//! section: 17.11
//! title: default of bool is false
//! rule: "`default(T)` produces the zero value of type T ... `bool`: `false`."
//! cites: 17.11
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(default(bool));
  end Main;
end Program;
