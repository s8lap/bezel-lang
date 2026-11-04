//! case: s2_3_0806
//! doc: spec
//! section: 2.3
//! title: a local named Task of a user class receives Delay and CompletedTask instead of the built-in Task
//! rule: every other always-available name may be shadowed by local declarations under the normal name-resolution rules
//! cites: 2.3, 2.7
//! expect: run
//! output: UD
//!   5
//! issue: 8019
module Conf;

public class T2
  public method Delay(n: int): string
    return "UD";
  end Delay;
  public property CompletedTask: int => 5;
end T2;

public class Program
  public static method Main()
    var Task := new T2();
    Console.WriteLine(Task.Delay(1));
    Console.WriteLine(Task.CompletedTask);
  end Main;
end Program;
