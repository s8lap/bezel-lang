//! case: s28_3_0092
//! doc: spec
//! section: 28.3
//! title: under the trust-admission report mode, a keys-based store reports nothing
//! rule: A compilation unit that emits no report under the trust-admission
//!   report mode carries no recorded trust clause: a SlotHandle stored in a
//!   field or a collection is a value, not a borrow, and no K4.3 clause
//!   attaches to it.
//! cites: 28.3, 28.11
//! expect: quiet
//! flags: --report-trust-admissions
module Conf;

public class Node
  public constructor()
  end constructor;
end Node;

public class Graph
  private owned _nodes: OwningSlotMap<Node>;
  private owned _edges: Array<SlotHandle>;

  public constructor()
    _nodes := new OwningSlotMap<Node>();
    _edges := new Array<SlotHandle>();
  end constructor;

  public method AddNode(): SlotHandle => _nodes.Insert(new Node());

  public method Link(a: SlotHandle, b: SlotHandle)
    _edges.Add(a);
    _edges.Add(b);
  end Link;
end Graph;
