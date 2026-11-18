//! case: s5_1_6706
//! doc: spec
//! section: 5.1
//! title: an interface-typed value converts to Object
//! rule: **any type to `Object`**: reference types (classes including the heap
//! cites: 11.2
//! expect: run
//! stdout: ok
module Conf;

public interface IMark
end IMark;

public class A: IMark
end A;

public class Program
  static method Log(o: Object)
    Console.WriteLine("ok");
  end Log;
  public static method Main()
    using var a := new A();
    var i: IMark := a;
    Program.Log(i);
  end Main;
end Program;
