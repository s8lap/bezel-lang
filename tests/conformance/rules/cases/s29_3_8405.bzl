//! case: s29_3_8405
//! doc: spec
//! section: 29.3
//! title: an iterator method of a generic class at a function-typed T yields each element once
//! rule: the body runs incrementally as the consumer calls `MoveNext()`
//! cites: 29.3, 14.1, 11.1
//! expect: run
//! stdout: 400
//! notes: Pins #9071 (class-level `T`, instance iterator). The class is instantiated at `() ->
//!   int` and the iterator's own parameter has type `T`; 2 yields per call, 400.
module Conf;
import System;
public class Box<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method It(p: T): IEnumerable<int>
    var r := lambda(): int => do var y := p; return 1; end;
    yield return r();
    yield return r();
  end It;
end Box;
public class Program
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var b := new Box<() -> int>(lambda(): int => 1);
      for var n in b.It(lambda(): int => 5) do t := t + n; end for;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
