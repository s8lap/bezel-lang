//! case: s30_13_0004
//! doc: spec
//! section: 30.13
//! title: two continuations of one domain never overlap, so an unsynchronized read-modify-write loses nothing
//! rule: **No intra-domain data races.** Two continuations of the same domain
//!   never access memory concurrently, so unsynchronized access to state
//!   shared between a domain's tasks is well-defined.
//! cites: 30.13
//! expect: run
//! stdout: 200000
//! notes: each continuation reads N once, counts without suspending, then
//!   writes N back; run-to-completion makes the two spans serial, so neither
//!   write is lost. An overlapping execution would print 100000.
module Conf;

import System.Threading;

public class Counter
  public N: int;
end Counter;

public class Program
  public static async method Bump(c: Counter, gate: Task<int>): Task
    var step := await gate;
    var v := c.N;
    var k := 0;
    for var i in 1..100000 do
      k := k + step;
    end;
    c.N := v + k;
  end Bump;

  public static async method Main(): Task
    using var c := new Counter();
    using var tcs := new TaskCompletionSource<int>();
    using var t1 := Bump(c, tcs.Task);
    using var t2 := Bump(c, tcs.Task);
    tcs.SetResult(1);
    await t1;
    await t2;
    Console.WriteLine(c.N);
  end Main;
end Program;
