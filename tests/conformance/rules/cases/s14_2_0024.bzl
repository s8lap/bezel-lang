//! case: s14_2_0024
//! doc: spec
//! section: 14.2
//! title: a call of the same interface generic method at a class type argument is admitted
//! rule: The type parameters of a dispatched generic method admit no move-only
//!   value type (§15.3) and no inline array of any form (§15.1) as a
//!   type argument.
//! cites: 14.2
//! expect: accept
module Conf;

public class Node
  public constructor()
  end constructor;
end Node;

public interface ISink
  method Put<T>(x: T);
end ISink;

public class Feeder
  public static method Feed(i: ISink)
    var arr: [2 owned]Node := [new Node(), new Node()];
    i.Put<Node>(arr[0]);
  end Feed;
end Feeder;
