//! case: s14_6_0002
//! doc: spec
//! section: 14.6
//! title: an in-SCC edge that nests a type parameter inside a larger argument is rejected
//! rule: Any substitution on an in-SCC edge other than a bare in-scope type
//!   parameter or a closed type is a compile error at the declaration holding
//!   the edge, with a diagnostic naming the cycle and the growing argument
//!   (§14.6).
//! cites: 14.6
//! expect: reject
//! diagnostic: cycle
module Conf;

public record Pair<A, B>(X: A, Y: B);

public class Node<T>
  private _child: Node<Pair<T, T>>?;

  public constructor()
  end constructor;
end Node;
