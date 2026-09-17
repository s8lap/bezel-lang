//! case: s11_3_0006
//! doc: spec
//! section: 11.3
//! title: an owned/plain parameter pair fails the declaration guard when it can collapse
//! rule: owned on an ordinary parameter distinguishes overloads exactly where
//!   §22.6's declaration guard admits the pair; a pair failing the guard
//!   clashes at the second declaration. where T: class does not qualify --
//!   it admits string and function-type substitutions, at which the marker
//!   is erased and the pair collapses.
//! cites: 11.3, 22.6
//! expect: reject
//! diagnostic: clash
module Conf;

public class Registry<T> where T: class
  public constructor()
  end constructor;

  public method Store(item: T)
  end Store;

  public method Store(owned item: T)
  end Store;
end Registry;
