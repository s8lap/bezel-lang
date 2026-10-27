//! case: s27_6_0032
//! doc: spec
//! section: 27.6
//! title: a bare Clear reference on an IReadOnlyCollection-typed receiver is rejected
//! rule: "the member resolved on the value exactly as the call `v.Scaled()` is" (§27.6, v0.340).
//!   `IReadOnlyCollection<out T>` declares only `pure readonly property Count: int` (stdlib §4.1);
//!   `Clear` is declared by `ICollection<T>`, the interface that *extends* `IReadOnlyCollection<T>`,
//!   and a receiver's declared type and its base types are the ones §26.5 rule 1 consults, so
//!   `ro.Clear` names no member at the static type `IReadOnlyCollection<int>` and is rejected as
//!   `ro.Clear()` is.
//! cites: 27.6, 26.5, 4.1
//! expect: reject
//! diagnostic: clear
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var ro: IReadOnlyCollection<int> := a;
    var f: () -> () := ro.Clear;
    Console.WriteLine(1);
  end Main;
end Program;
