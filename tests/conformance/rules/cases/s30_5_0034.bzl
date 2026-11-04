//! case: s30_5_0034
//! doc: spec
//! section: 30.5
//! title: an OwningTask disposed without being awaited disposes the result it produced
//! rule: If a `OwningTask<T>` is dropped or disposed before its result has
//!   been transferred — for example, by `using var` on the task itself with no
//!   await — disposing the task disposes the result if one was produced
//! cites: 30.5, 30.10
//! expect: run
//! output: dispose x
//!   after
module Conf;

public class Res
  private _n: string;
  public constructor(n: string) _n := n; end constructor;

  destructor
    Console.WriteLine($"dispose {_n}");
  end destructor;
end Res;

public class Program
  public static method Main()
    do
      using var t := Task.FromOwnedResult<Res>(new Res("x"));
    end do;
    Console.WriteLine("after");
  end Main;
end Program;
