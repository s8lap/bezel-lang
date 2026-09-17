//! case: s21_8_0002
//! doc: spec
//! section: 21.8
//! title: a bare re-raise outside a catch arm is a compile error
//! rule: The bare re-raise is valid only inside a catch arm body. Bare
//!   `throw` (either form) outside a catch arm body is a compile error.
//! cites: 21.8, 31.2.4
//! expect: reject
//! diagnostic: catch
module Conf;

public class Program
  public static method Main()
    throw;
  end Main;
end Program;
