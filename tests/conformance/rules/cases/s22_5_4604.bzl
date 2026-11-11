//! case: s22_5_4604
//! doc: spec
//! section: 22.5
//! title: naming a parameter already bound positionally in a built-in call is a compile error
//! rule: Each supplied argument binds exactly one parameter and each parameter is bound by at most one supplied argument
//! cites: 9.1
//! expect: reject
//! diagnostic: bound more than once
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("ff", s := "1"));
  end Main;
end Program;
