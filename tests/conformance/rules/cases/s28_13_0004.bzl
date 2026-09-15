//! case: s28_13_0004
//! doc: spec
//! section: 28.13
//! title: the compiler does not detect ownership cycles through Shared<T>
//! rule: Cycles leak. A value that (transitively, through owned fields)
//!   holds a `Shared<T>` handle participating in its own ownership keeps
//!   its count above zero forever. The compiler does not detect ownership
//!   cycles through `Shared<T>`.
//! cites: 28.13
//! expect: run
//! stdout: n
module Conf;

public class Node
  public property Payload: string;
  private owned _next: Shared<Node>?;

  public constructor(payload: string)
    Payload := payload;
    _next := null;
  end constructor;

  public method Attach(owned n: Shared<Node>)
    _next := n;
  end Attach;
end Node;

public class Program
  public static method Main()
    using var self := new Shared<Node>(new Node("n"));
    self.Get().Attach(self.Clone());   // stores a clone of `self` into the
                                        // node's own field: a self-cycle
    Console.WriteLine(self.Get().Payload);
  end Main;
end Program;
