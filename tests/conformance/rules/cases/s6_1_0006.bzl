//! case: s6_1_0006
//! doc: spec
//! section: 6.1
//! title: a compile-time constant that overflows is rejected, never deferred to runtime
//! rule: An evaluation fault — integer or decimal overflow, division or mod by
//!   zero, a shift count outside the §6.5 domain — is a compile error at the
//!   constant expression, never a deferred runtime fault: const B :=
//!   2147483647 + 1;.
//! cites: 6.1, 17.14
//! expect: reject
//! diagnostic: compile error at the constant expression
module Conf;

public class Program
  public static method Make()
    const B := 2147483647 + 1;
  end Make;
end Program;
