//! case: s17_14_0011
//! doc: spec
//! section: 17.14
//! title: negating a constant whose value is the int minimum is a compile error at the constant expression
//! rule: An evaluation **fault** — integer or `decimal` overflow, division or `mod` by zero, a shift count outside the §6.5 domain — is a **compile error at the constant expression**, never a deferred runtime fault.
//! cites: 17.14, 6.1
//! expect: reject
//! diagnostic: no representable result
module Conf;

public class Program
  public static method Main()
    const N: int := -2147483647 - 1;
    Console.WriteLine(-N);
  end Main;
end Program;
