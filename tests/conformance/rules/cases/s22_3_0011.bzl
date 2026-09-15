//! case: s22_3_0011
//! doc: spec
//! section: 22.3
//! title: out is forbidden on a lambda's parameters
//! rule: "`ref`/`out` are forbidden on `async` methods, on lambdas, on
//!   iterator methods and on indexers."
//! cites: 22.3
//! expect: reject
//! diagnostic: lambda
module Conf;

public class Program
  public static method Main()
    var f := lambda(out x: int) => do x := 1; end;
  end Main;
end Program;
