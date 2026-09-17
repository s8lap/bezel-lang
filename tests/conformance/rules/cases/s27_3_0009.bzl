//! case: s27_3_0009
//! doc: spec
//! section: 27.3
//! title: no parameter modifier composes with the bare ignore
//! rule: No parameter modifier and no type annotation compose with the bare
//!   ignore — `FuncParamMods` with `_` is rejected, because a `ref` parameter
//!   reads and writes through its operand, which an ignore cannot supply.
//! cites: 27.3
//! expect: reject
//! diagnostic: ignore
module Conf;

public class Program
  public method Demo()
    var f: (ref int) -> () := lambda(ref _) => do Console.WriteLine("x"); end;
  end Demo;
end Program;
