//! case: s28_2_0017
//! doc: spec
//! section: 28.2
//! title: a nested class with an optional-parameter method of another name is accepted
//! rule: The rule of §28.2 is keyed on the name `Dispose`; a method `Release(mode:
//!   int := 0)` with the same nesting and parameter list is an ordinary member.
//!   This is the accepting twin of s28_2_0016.
//! cites: 22.5, 28.2
//! expect: accept
module Conf;

public class Outer
  public class C
    public method Release(mode: int := 0)
    end Release;
  end C;
end Outer;
