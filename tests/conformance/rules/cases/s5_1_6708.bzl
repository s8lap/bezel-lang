//! case: s5_1_6708
//! doc: spec
//! section: 5.1
//! title: a class converts to an interface it implements
//! rule: **a type to an interface it implements or conforms to**: classes by
//! cites: 11.2
//! expect: run
//! stdout: ok
module Conf;

public interface IMark
end IMark;

public class A: IMark
end A;

public class Program
  static method Log(m: IMark)
    Console.WriteLine("ok");
  end Log;
  public static method Main()
    using var a := new A();
    var i: IMark := a;
    Program.Log(i);
    Program.Log(a);
  end Main;
end Program;
