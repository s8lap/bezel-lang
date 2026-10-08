//! case: s14_2_0023
//! doc: spec
//! section: 14.2
//! title: a dispatched generic call at an owning inline array type argument is rejected at the call
//! rule: The type parameters of a dispatched generic method admit no move-only
//!   value type (§15.3) and no inline array of any form (§15.1) as a
//!   type argument. A call of one at such an argument ... is a **compile
//!   error at the call**
//! cites: 14.2, 15.3, 15.1.1
//! expect: reject
//! diagnostic: inline array
//! issue: 5640
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
    i.Put<[2 owned]Node>(arr);
  end Feed;
end Feeder;
