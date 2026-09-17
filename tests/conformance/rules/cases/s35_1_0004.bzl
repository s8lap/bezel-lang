//! case: s35_1_0004
//! doc: spec
//! section: 35.1
//! title: a payload field of the choice's own type is rejected
//! rule: A choice payload field may not, directly or transitively through value-typed
//!   components, have the declaring choice's own type: a value type cannot contain
//!   itself.
//! expect: reject
//! diagnostic: contain itself
module Conf;

public choice Tree
  Leaf,
  Node(left: Tree, right: Tree);   // ERROR: payload is the declaring choice's own type
end Tree;
