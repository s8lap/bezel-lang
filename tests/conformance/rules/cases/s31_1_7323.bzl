//! case: s31_1_7323
//! doc: spec
//! section: 31.1
//! title: a lambda throws the result of an `owned`-returning call, bound or
//!   direct
//! rule: `throw expr` requires `expr` to be an owned instance of an
//!   `Exception`-derived class type
//! cites: 28.3
//! expect: run
//! output: caught a6
//!   caught a6b
//! notes: Pins the #8781 fix on the admitting side: `Make()` returns `owned
//!   R`, so both `var e := Make(); throw e;` and the direct `throw Make()`
//!   carry an owned operand.
module Conf;

public class R: Exception
  public constructor() end constructor;
end R;

public class Cell
  public value: int := 0;
  public constructor(v: int) this.value := v; end constructor;
end Cell;

public interface IShape method Area(): int; end IShape;

public class Sq: IShape
  public constructor() end constructor;
  public method Area(): int => 4;
end Sq;

public class Maker
  public static method Make(): owned R
    return new R();
  end Make;
end Maker;

public class Program
  public static method Main(): int
    var f := lambda(): int => do var e := Maker.Make(); throw e; end;
    try _ := f(); catch var x: R => Console.WriteLine("caught a6"); end;
    var g := lambda(): int => throw Maker.Make();
    try _ := g(); catch var y: R => Console.WriteLine("caught a6b"); end;
    return 0;
  end Main;
end Program;
