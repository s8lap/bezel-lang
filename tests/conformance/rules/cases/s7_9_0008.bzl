//! case: s7_9_0008
//! doc: spec
//! section: 7.9
//! title: a field of an enum type with no zero-valued constant needs an initializer
//! rule: A field whose type is not defaultable requires an explicit initializer:
//!   a field initializer, or assignment in every constructor. The compiler emits
//!   a definite-assignment error if neither path supplies a value. An enum type
//!   with no declared constant of underlying value 0 is not defaultable.
//! cites: 17.11
//! expect: reject
//! diagnostic: every constructor
module Conf;

public enum Level
  Low := 1, High := 2;
end Level;

public class Config
  private _level: Level;

  public constructor()
  end constructor;
end Config;
