//! case: s14_6_0001
//! doc: spec
//! section: 14.6
//! title: self-referential recursion at the same bare type parameter is legal
//! rule: Recursion at the same arguments stays legal: an edge substituting a
//!   type parameter of its target with a bare type parameter in scope at the
//!   source declaration is within the bounded-instantiation-closure rule
//!   (§14.6).
//! cites: 14.6
//! expect: accept
module Conf;

public class Node<T>
  private _child: Node<T>?;

  public constructor()
  end constructor;
end Node;
