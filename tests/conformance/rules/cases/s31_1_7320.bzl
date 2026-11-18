//! case: s31_1_7320
//! doc: spec
//! section: 31.1
//! title: a lambda body throws an owned local declared with `var`
//! rule: `throw expr` requires `expr` to be an owned instance of an
//!   `Exception`-derived class type
//! cites: 28.3, 21.8
//! expect: run
//! stdout: caught a1
//! notes: Pins the #8781 fix on the admitting side: the operand `c` is a
//!   lambda-local owned binding (`var c := new R();`), an owned value, so
//!   `throw c;` is admitted inside the lambda body and the handler runs.
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

public class Program
  public static method Main(): int
    var f := lambda(): int => do var c := new R(); throw c; end;
    try _ := f(); catch var x: R => Console.WriteLine("caught a1"); end;
    return 0;
  end Main;
end Program;
