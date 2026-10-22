//! case: s31_1_0015
//! doc: spec
//! section: 31.1
//! title: a throw of a borrowed (non-owned) class parameter is rejected
//! rule: "The operand is consumed as an owned value under the ownership rules below"
//! cites: 31.1
//! expect: reject
//! diagnostic: owned
module Conf;

public class Program
  public static method M<T>(x: T) where T: Exception
    throw x;
  end M;

  public static method Main()
    M(new IOException("m"));
  end Main;
end Program;
