//! case: s14_2_0041
//! doc: spec
//! section: 14.2
//! title: an inferred type argument at a dispatched generic call rejects an owning inline array
//! rule: The type parameters of a dispatched generic method admit no move-only
//!   value type (§15.3) and no inline array of any form (§15.1) as a type
//!   argument. This rule applies to INFERRED type arguments (§14.5) as well
//!   as explicit ones (§14.2) — the copy-for-dispatch problem exists
//!   regardless of how the type argument was supplied.
//! cites: 14.2, 14.5, 15.1, 15.3
//! expect: reject
//! diagnostic: inline array
//! issue: 5685
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
    i.Put(arr);  // T inferred as [2 owned]Node — should be rejected per §14.2
  end Feed;
end Feeder;
