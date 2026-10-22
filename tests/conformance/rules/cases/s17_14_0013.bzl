//! case: s17_14_0013
//! doc: spec
//! section: 17.14
//! title: subtracting one from the int minimum constant is a compile error at the constant expression
//! rule: An evaluation **fault** — integer or `decimal` overflow, division or `mod` by zero, a shift count outside the §6.5 domain — is a **compile error at the constant expression**, never a deferred runtime fault.
//! cites: 17.14, 6.1
//! expect: reject
//! diagnostic: overflow
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.MinValue - 1);
  end Main;
end Program;
