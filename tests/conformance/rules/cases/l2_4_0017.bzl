//! case: l2_4_0017
//! doc: stdlib
//! section: 2.4
//! title: Variables result is an owned value that a method may return and a caller may bind
//! rule: public static method Variables(): owned Dictionary<string, string>;
//! expect: run
//! stdout: True
//! issue: 7949
//! notes: The declared result is owned, so it may be returned through an owned result and bound by the caller.
module Conf;

public class Program
  private static method Make(): owned Dictionary<string, string>
    return Environment.Variables();
  end Make;

  public static method Main()
    using var d := Make();
    Console.WriteLine(d.Count >= 0);
  end Main;
end Program;
