//! case: s9_8_0004
//! doc: spec
//! section: 9.8
//! title: serialized optional on a non-defaultable enum component is rejected
//! rule: A serialized optional component whose type is not defaultable is a
//!   compile error at the conformance declaration; a missing field would have no
//!   value to supply.
//! cites: 17.11
//! expect: reject
//! diagnostic: defaultable
module Conf;

public enum Level
  Low := 1,
  High := 2;
end Level;

public record Setting: ISerializable, IDeserializable
  public readonly property Mode: Level serialized optional;

  public constructor(mode: Level)
    Mode := mode;
  end constructor;
end Setting;
