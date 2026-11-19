//! case: s28_9_7727
//! doc: spec
//! section: 28.9
//! title: a function-typed getter of a record read through its interface yields the component closure
//! rule: a getter never transfers a reclamation obligation to its reader
//! cites: 23.3
//! expect: run
//! stdout: 5
module Conf;
import System;
interface IFn
  readonly property P: () -> int;
end IFn;
record R(f: () -> int) : IFn
  public property P: () -> int
    get => do return f; end;
  end P;
end R;
class C
  public static method Main(): int
    var g := lambda(): int => 5;
    var r := new R(g);
    var fn: IFn := r;
    var x := fn.P;
    Console.WriteLine(x());
    return 0;
  end Main;
end C;
