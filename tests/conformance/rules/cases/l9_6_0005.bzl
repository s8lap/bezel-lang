//! case: l9_6_0005
//! doc: stdlib
//! section: 9.6
//! title: IsNullOrWhiteSpace(null) is true
//! rule: static pure method IsNullOrWhiteSpace(value: string?): bool;
//! cites: 9.6
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var s: string? := null;
    Console.WriteLine(string.IsNullOrWhiteSpace(s));
  end Main;
end Program;
