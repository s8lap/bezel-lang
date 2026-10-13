//! case: s28_3_0122
//! doc: spec
//! section: 28.3
//! title: a receiver-sourced borrow of the binding as an earlier argument freezes the later move
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

public class Leaf
  public property N: int;

  public constructor()
  end constructor;
end Leaf;

public class Node
  private owned _child: Leaf := new Leaf();

  public method Child(): Leaf => _child;
end Node;

public class Worker
  public static method Sink(view: Leaf, owned n: Node)
    Console.WriteLine(view.N);
  end Sink;

  public static method Run()
    var x := new Node();
    Sink(x.Child(), move x);
  end Run;
end Worker;
