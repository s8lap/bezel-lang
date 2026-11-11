//! case: s7_7_4704
//! doc: spec
//! section: 7.7
//! title: a ref string? parameter ??= int is rejected
//! rule: for a left operand of type `T?`, the right operand must be implicitly convertible
//! cites: 6.6, 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Fill(ref s: string?, v: int)
    s ??= v;
  end Fill;

  public static method Main()
    var s: string? := null;
    Program.Fill(ref s, 5);
    Console.WriteLine(s ?? "none");
  end Main;
end Program;
