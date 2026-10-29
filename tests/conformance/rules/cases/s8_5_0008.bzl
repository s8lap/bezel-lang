//! case: s8_5_0008
//! doc: spec
//! section: 8.5
//! title: a call returning a null function-typed value renders null at a hole
//! rule: A null hole renders as the four characters null with no dispatch; a nullable
//!   function-typed call result that is null is such a hole (spec 8.5, 18.5).
//! cites: 18.5
//! expect: run
//! output: null
module Conf;

public class Program
  static method MkF(n: int): ((int) -> int)?
    if n mod 2 = 0 then return null; end;
    return lambda(x: int) => x + n;
  end MkF;

  public static method Main()
    Console.WriteLine($"{MkF(2)}");
  end Main;
end Program;
