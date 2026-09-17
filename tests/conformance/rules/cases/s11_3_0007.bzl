//! case: s11_3_0007
//! doc: spec
//! section: 11.3
//! title: an owned/plain parameter pair is legal when the constraint excludes every collapsing substitution
//! rule: An interface bound qualifies exactly when the interface's conforming
//!   set excludes every value-kind and runtime-managed type as a matter of
//!   language rule -- where T: IDisposable qualifies, its closure guaranteed
//!   by §8.7.
//! cites: 11.3, 22.6
//! expect: accept
module Conf;

public class Registry<T> where T: IDisposable
  public constructor()
  end constructor;

  public method Store(item: T)
  end Store;

  public method Store(owned item: T)
  end Store;
end Registry;
