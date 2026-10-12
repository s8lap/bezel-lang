//! case: s30_11_0005
//! doc: spec
//! section: 30.11
//! title: a rejected TrySetResult disposes the result it was handed, leaving only the task's own
//! rule: If `TrySetResult` returns `false` because the task is already in a
//!   terminal state, the implementation disposes `result` before returning.
//!   The caller has no further obligation — ownership was transferred at the
//!   call, and the implementation is responsible for releasing it on every
//!   path.
//! cites: 30.11, 28.3
//! expect: run
//! output: False
//!   1
//!   0
module Conf;

import System.Threading;

public class Res
  public static n: int := 0;
  public constructor()
    Res.n := Res.n + 1;
  end constructor;
  destructor
    Res.n := Res.n - 1;
  end destructor;
end Res;

public class Program
  public static method Main()
    do
      using var tcs := new OwningTaskCompletionSource<Res>();
      tcs.TrySetResult(new Res());
      Console.WriteLine(tcs.TrySetResult(new Res()));
      Console.WriteLine(Res.n);
    end do;
    Console.WriteLine(Res.n);
  end Main;
end Program;
