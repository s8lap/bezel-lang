//! case: s17_14_0007
//! doc: spec
//! section: 17.14
//! title: a shift count outside the domain at a constant expression is a compile error at the declaration
//! rule: "a shift count outside the §6.5 domain — is a compile error at the
//!   constant expression ... `const F := 1 << 64;` ... rejected where they
//!   are written."
//! cites: 17.14, 6.5
//! expect: reject
//! diagnostic: shift
module Conf;

public class Program
  public static method Main()
    const f := 1 << 64;
  end Main;
end Program;
