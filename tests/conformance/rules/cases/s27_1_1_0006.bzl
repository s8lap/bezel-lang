//! case: s27_1_1_0006
//! doc: spec
//! section: 27.1.1
//! title: modifier slots must match exactly between two function-typed values
//! rule: Modifier slots (`owned`/`ref`/`out`/`out owned`/`scoped`) must match
//!   exactly (§27.1); there is no conversion between a modified and an
//!   unmodified parameter slot.
//! cites: 27.1.1, 27.1
//! expect: reject
//! diagnostic: modifier
module Conf;

public class Program
  public method Demo()
    var plain: (int) -> () := lambda(x: int) => Console.WriteLine($"{x}");
    var byRef: (ref int) -> () := plain;
  end Demo;
end Program;
