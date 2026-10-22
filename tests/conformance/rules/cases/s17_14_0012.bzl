//! case: s17_14_0012
//! doc: spec
//! section: 17.14
//! title: a constant ulong product that overflows is a compile error at the constant expression
//! rule: An evaluation **fault** — integer or `decimal` overflow, division or `mod` by zero, a shift count outside the §6.5 domain — is a **compile error at the constant expression**, never a deferred runtime fault.
//! cites: 17.14, 6.1
//! expect: reject
//! diagnostic: overflow
module Conf;

public class Program
  public static method Main()
    const N: ulong := 9223372036854775808UL;
    Console.WriteLine(N * 2UL - 1UL);
  end Main;
end Program;
