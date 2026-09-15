//! case: s11_7_0003
//! doc: spec
//! section: 11.7
//! title: a static class may not declare an instance field
//! rule: A static class may not have instance fields, instance methods,
//!   instance properties, indexers, constructors (other than a static
//!   constructor), or destructors.
//! cites: 11.7
//! expect: reject
//! diagnostic: static
module Conf;

public static class Utils
  private _count: int := 0;
end Utils;
