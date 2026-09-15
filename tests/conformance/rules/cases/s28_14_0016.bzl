//! case: s28_14_0016
//! doc: spec
//! section: 28.14
//! title: scoped on a static method with no owned return is a compile error
//! rule: A scoped parameter on a static method is admitted only when the
//!   method's return is owned; there is no receiver, no constructed
//!   object, and no instance to hold the stored value otherwise.
//! cites: 28.14
//! expect: reject
//! diagnostic: owned
module Conf;

public class Registry
  public static method Subscribe(scoped cb: () -> ())
  end Subscribe;
end Registry;
