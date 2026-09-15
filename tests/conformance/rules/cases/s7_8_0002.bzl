//! case: s7_8_0002
//! doc: spec
//! section: 7.8
//! title: null-forgiving on a null value raises NullReferenceException at runtime
//! rule: If `x` is null at runtime, `NullReferenceException` is raised.
//! expect: run
//! stdout: caught null
module Conf;

public class Program
  public static method Main()
    var w: string? := null;
    try
      var s := w!;
      Console.WriteLine(s);
    catch
      var ex: NullReferenceException => Console.WriteLine("caught null")
    end try;
  end Main;
end Program;
