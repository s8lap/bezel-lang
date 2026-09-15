//! case: s17_14_0005
//! doc: spec
//! section: 17.14
//! title: integer overflow at a constant expression is a compile error at the declaration
//! rule: "An evaluation **fault** — integer or `decimal` overflow, division
//!   or `mod` by zero, a shift count outside the §6.5 domain — is a
//!   **compile error at the constant expression**, never a deferred runtime
//!   fault: `const B := 2147483647 + 1;` ... are rejected where they are
//!   written."
//! cites: 17.14, 6.1
//! expect: reject
//! diagnostic: overflow
module Conf;

public class Program
  public static method Main()
    const b := 2147483647 + 1;
  end Main;
end Program;
