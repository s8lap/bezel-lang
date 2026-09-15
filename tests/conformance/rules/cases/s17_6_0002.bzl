//! case: s17_6_0002
//! doc: spec
//! section: 17.6
//! title: postfix ++ as a statement increments the operand by one
//! rule: "`var c := 5; c++; // c is 6`"
//! cites: 17.6
//! expect: run
//! stdout: 6
module Conf;

public class Program
  public static method Main()
    var c := 5;
    c++;
    Console.WriteLine(c);
  end Main;
end Program;
