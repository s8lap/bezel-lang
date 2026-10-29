//! case: s26_5_0047
//! doc: spec
//! section: 26.5
//! title: an extension CountOf is reached through a receiver narrowed from string? to string
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the current module and all imported modules
//! cites: 26.5, 7.5
//! expect: run
//! output: 9
module Conf;

extension string
  public method CountOf(sub: string): int => 9;
end;

public class Program
  public static method Main()
    var s: string? := "a,b";
    if s <> null then
      Console.WriteLine(s.CountOf(","));
    end;
  end Main;
end Program;
