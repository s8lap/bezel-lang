//! case: s27_3_0044
//! doc: spec
//! section: 27.3
//! title: a procedure lambda does not convert to a value-returning function type
//! rule: a procedure-bodied lambda does not convert to a value-returning function type, nor the reverse (v0.338 §27.3)
//! cites: 27.3
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var w := lambda() => do end;
    var f: () -> int := w;
  end Main;
end Program;
