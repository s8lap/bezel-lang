//! case: s5_3_0002
//! doc: spec
//! section: 5.3
//! title: casting a null nullable to its non-nullable form raises NullReferenceException
//! rule: var m: int := int(n); // raises NullReferenceException if n is null.
//! cites: 5.3
//! expect: run
//! stdout: nullref
module Conf;

public class Program
  public static method Main()
    try
      var n: int? := null;
      var m := int(n);
      Console.WriteLine(m);
    catch
      var ex: NullReferenceException => Console.WriteLine("nullref");
    end;
  end Main;
end Program;
