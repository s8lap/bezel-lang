//! case: l9_6_0053
//! doc: stdlib
//! section: 9.6
//! title: string Replace binds newChar/oldChar and newValue/oldValue by name in either order
//! rule: public pure method Replace(oldChar: char, newChar: char): string; ... public pure method Replace(oldValue: string, newValue: string): string;
//! cites: 9.6, 22.5
//! expect: run
//! output: hello Jorld
//!   hJllo world
module Conf;

public class Program
  public static method Main()
    var s := "hello world";
    Console.WriteLine(s.Replace(newValue := "J", oldValue := "w"));
    Console.WriteLine(s.Replace(newChar := 'J', oldChar := 'e'));
  end Main;
end Program;
