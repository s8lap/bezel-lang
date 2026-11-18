//! case: s5_1_6707
//! doc: spec
//! section: 5.1
//! title: a class deriving from Exception converts to Exception
//! rule: **class to base class**, transitively, up to `Object`
//! cites: 31.4
//! expect: run
//! stdout: Log: hello
module Conf;

public class A: Exception
  public constructor(m: string) base(m); end constructor;
end A;

public class Program
  static method Log(ex: Exception)
    Console.WriteLine($"Log: {ex.Message}");
  end Log;
  public static method Main()
    using var a := new A("hello");
    Program.Log(a);
  end Main;
end Program;
