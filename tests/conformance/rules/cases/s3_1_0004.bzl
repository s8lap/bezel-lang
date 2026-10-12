//! case: s3_1_0004
//! doc: spec
//! section: 3.1
//! title: function types are reference types
//! rule: Function types — (T1, ..., Tn) -> R, reference types.
//! expect: run
//! stdout: lambda 1
module FnType;

public class FnType
  public static method Main()
    var f: (int) -> string := lambda (x) => $"lambda {x}";
    Console.WriteLine(f(1));
  end Main;
end FnType;
