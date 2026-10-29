//! case: s28_2_0015
//! doc: spec
//! section: 28.2
//! title: a record nested in a class with an ordinary method name is accepted
//! rule: The rule of §28.2 is keyed on the name `Dispose`: "a member named
//!   `Dispose`" is the compile error, so a member of the same record with another
//!   name is an ordinary method. This is the accepting twin of s28_2_0014.
//! cites: 28.2
//! expect: accept
module Conf;

public class Outer
  public record R
    public method Close()
    end Close;
  end R;
end Outer;
