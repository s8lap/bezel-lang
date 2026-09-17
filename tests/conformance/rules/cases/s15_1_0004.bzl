//! case: s15_1_0004
//! doc: spec
//! section: 15.1
//! title: a zero-size inline array is a compile error
//! rule: N must be greater than zero; `[0]T` is a compile error (§15.1).
//! cites: 15.1
//! expect: reject
//! diagnostic: zero
module Conf;

public class Program
  public static method Foo()
    var x: [0]int;
  end Foo;
end Program;
