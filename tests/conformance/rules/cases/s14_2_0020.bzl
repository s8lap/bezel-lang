//! case: s14_2_0020
//! doc: spec
//! section: 14.2
//! title: a member where clause constraining its own type parameter, bound to an enclosing one, is admitted
//! rule: A clause constraining the member's own parameter is unaffected,
//!   including where its bound names an enclosing parameter (`method M<U>()
//!   where U: T` on `class C<T>`) — the ordinary constraint form of §14.2
//!   (§14.2 rule 1, v0.277).
//! cites: 14.2
//! expect: accept
module Conf;

public class Container<T>
  public method Store<U>(item: U) where U: T
    Console.WriteLine("stored");
  end Store;

  public constructor()
  end constructor;
end Container;
