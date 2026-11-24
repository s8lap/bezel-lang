//! case: s27_5_8301
//! doc: spec
//! section: 27.5
//! title: a lambda in an expression-bodied indexer getter reads its own call's index under h[i]
//!   += 1
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 6.6, 23.4
//! expect: run
//! stdout: 1425
//! notes: Pins #8941 (the issue's repro). Each get call binds its own `i`; the lambda `k`
//!   formed after the call `g()` reads that call's `i`. Per pass the getter returns i + 3,
//!   `+= 1` makes the stored value i + 4, and the setter adds it to n. Hand sum over i =
//!   0..49 of (i + 4) = 1225 + 200 = 1425.
module Conf;
import System;
public class H
  public n: int;
  public property [i: int]: int
    get => (var g := lambda(): int => 3; var q := g(); var k := lambda(): int => i + q; k());
    set => n := n + value;
  end;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var i := 0;
    while i < 50 do h[i] += 1; i := i + 1; end;
    Console.WriteLine(h.n);
    return 0;
  end Main;
end Program;
