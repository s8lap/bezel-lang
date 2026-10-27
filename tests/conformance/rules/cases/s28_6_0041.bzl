//! case: s28_6_0041
//! doc: spec
//! section: 28.6
//! title: the owner of an owned local may dispose it
//! rule: "Explicit disposal — `x.Dispose()` ... — requires a receiver the disposer
//!   owns: an owned local or `owned` parameter, a `using var`, an unbound owned
//!   temporary, a `move` result, or an `owned` field" (§28.6, v0.340). `a` is the
//!   owner of the collection literal it was bound to (§28.3's producers table), so
//!   `a.Dispose()` is a disposal by the owner and is admitted; the scope-end pass
//!   finds the binding already disposed and disposes nothing a second time
//!   (§28.2's idempotence), so the program ends normally.
//! cites: 28.6, 28.3, 28.2
//! expect: run
//! stdout: ok
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    a.Dispose();
    Console.WriteLine("ok");
  end Main;
end Program;
