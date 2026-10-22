//! case: s17_14_0014
//! doc: spec
//! section: 17.14
//! title: an overflowing constant in an if statement with a run-time condition is a compile error
//! rule: An evaluation **fault** — integer or `decimal` overflow, division or `mod` by zero, a shift count outside the §6.5 domain — is a **compile error at the constant expression**, never a deferred runtime fault.
//! cites: 17.14, 6.1
//! expect: reject
//! diagnostic: overflow
module Conf;

public class Program
  public static method Main()
    var x: int := 3;
    if x > 100 then
      Console.WriteLine(2147483647 + 1);
    end if;
  end Main;
end Program;
