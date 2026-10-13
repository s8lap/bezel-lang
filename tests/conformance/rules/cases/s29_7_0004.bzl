//! case: s29_7_0004
//! doc: spec
//! section: 29.7
//! title: an iterator body fault disposes the owned parameter before the handler and completes the machine
//! rule: A **body fault** reaches the same state by the same pass: an
//!   exception that escapes the body out of `MoveNext()` ... is the frame's
//!   **completion** — the pending cleanup actions active at the throw run in
//!   the unwind in reverse order of establishment ..., the frame's
//!   undischarged `owned` and move-only parameters after them (§29.8), and the
//!   machine is completed before the exception reaches the consumer; a later
//!   `MoveNext()` returns `false`
//! cites: 29.7, 29.8, 28.3
//! expect: run
//! output: closed 1
//!   caught fault
//!   False
//! issue: 5825
module Conf;

public class Res
  public property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  destructor
    Console.WriteLine($"closed {N}");
  end destructor;
end Res;

public class Demo
  public static method Gen(owned r: Res): owned IEnumerator<int>
    yield return r.N;
    throw new InvalidOperationException("fault");
  end Gen;

  public static method Main()
    using var e := Gen(new Res(1));
    _ := e.MoveNext();
    try
      _ := e.MoveNext();
    catch
      var ex: InvalidOperationException => Console.WriteLine($"caught {ex.Message}")
    end try;
    Console.WriteLine(e.MoveNext());
  end Main;
end Demo;
