//! case: s5_1_6709
//! doc: spec
//! section: 5.1
//! title: a class converts to the compiler-generated IDisposable
//! rule: the compiler-generated `IDisposable`
//! cites: 28.2
//! expect: run
//! stdout: Log
module Conf;

public class A
  destructor Console.WriteLine("D"); end destructor;
end A;

public class Program
  static method Log(d: IDisposable)
    Console.WriteLine("Log");
  end Log;
  public static method Main()
    using var a := new A();
    Program.Log(a);
  end Main;
end Program;
