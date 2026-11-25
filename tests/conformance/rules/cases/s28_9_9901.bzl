//! case: s28_9_9901
//! doc: spec
//! section: 28.9
//! title: Peek()() and Dequeue()() on a Queue of closures both reach the stored function value
//! rule: Runtime management governs the *function value itself*
//! cites: 28.9, 28.11, 4.11
//! expect: run
//! stdout: 803
//! issue: 9112
//! notes: Harvested from the #9104/#9112 review (v13_queue_fresh_peek_then_dequeue). Function values are
//!   runtime-managed (28.9): the borrow Peek hands out and the element Dequeue hands out are the same
//!   value, so both calls return 2 (4 per pass, 800 over 200 passes), and the unrelated field closure
//!   `keep.cb` is still callable afterwards (+3): 803.
module Conf;

public class H
  public cb: () -> int := lambda() => 3;
end H;

public class C
  public static method Main()
    var total := 0;
    var keep := new H();
    for var i in 0 ..< 200 do
      using var q: Queue<() -> int> := new Queue<() -> int>();
      q.Enqueue(lambda() => 2);
      total := total + q.Peek()() + q.Dequeue()();
    end for;
    total := total + keep.cb();
    Console.WriteLine(total);
  end Main;
end C;
