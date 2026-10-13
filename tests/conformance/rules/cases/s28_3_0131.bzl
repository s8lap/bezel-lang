//! case: s28_3_0131
//! doc: spec
//! section: 28.3
//! title: a later argument reading a binding an earlier argument moved is a moved-from read
//! rule: **moved-from** — the obligation has transferred out; the binding is
//!   unreadable until reassigned, and discharge machinery skips it
//! cites: 28.3, 22.5
//! expect: reject
//! diagnostic: moved-from
//! issue: 5813
module Conf;

public class Node
  public property Value: int;

  public constructor()
  end constructor;
end Node;

public class Pair
  public property Total: int;

  public constructor(owned n: Node, view: Node)
    Total := view.Value;
  end constructor;
end Pair;

public class Demo
  public static method Run()
    var n := new Node();
    using var p := new Pair(move n, n);
  end Run;
end Demo;
