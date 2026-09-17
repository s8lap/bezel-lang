//! case: s21_9_0023
//! doc: spec
//! section: 21.9
//! title: a fluent method returning a base of the receiver's type stays warned
//! rule: A fluent method whose declared return is a *base* of the receiver's type
//!   stays warned — conservative by design; the exemption requires the result's
//!   static type to equal the receiver's static type.
//! cites: 21.9
//! expect: warn
//! diagnostic: discarded
module Conf;

public class Node
  public constructor()
  end constructor;
end Node;

public class Leaf: Node
  public constructor()
  end constructor;

  public method Tag(): Node => this;
end Leaf;

public class Builder
  public method Build(leaf: Leaf)
    leaf.Tag();
  end Build;
end Builder;
