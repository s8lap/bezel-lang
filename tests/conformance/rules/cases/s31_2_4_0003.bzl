//! case: s31_2_4_0003
//! doc: spec
//! section: 31.2.4
//! title: a bare re-raise outside a catch arm body is rejected
//! rule: Bare `throw` (either form) outside a catch arm body is a compile
//!   error. It is valid only inside a catch arm body.
//! cites: 31.2.4
//! expect: reject
//! diagnostic: catch arm
module Conf;

public class Program
  public static method Main()
    try
      throw;
    finally
      Console.WriteLine("finally");
    end try;
  end Main;
end Program;
