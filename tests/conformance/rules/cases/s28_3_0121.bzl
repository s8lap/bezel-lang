//! case: s28_3_0121
//! doc: spec
//! section: 28.3
//! title: a borrow passed as an earlier argument freezes the binding a later argument moves
//! rule: A borrow of the consumed binding passed as an argument or operand
//!   evaluated **before** a consuming argument of the same call (§22.5's
//!   textual order) is such a borrow, its use being the call itself ...
//!   `Sink(x, move x)` and `Sink(x.Child(), move x)` are rejected at the
//!   `move`
//! cites: 28.3, 22.5
//! expect: reject
//! diagnostic: live borrow
//! issue: 5811
module Conf;

public class Counter
  public property N: int;

  public constructor()
  end constructor;
end Counter;

public class Worker
  public static method Sink(view: Counter, owned c: Counter)
    Console.WriteLine(view.N);
  end Sink;

  public static method Run()
    var x := new Counter();
    Sink(x, move x);
  end Run;
end Worker;
