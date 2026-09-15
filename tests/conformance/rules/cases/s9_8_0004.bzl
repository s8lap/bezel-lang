//! case: s9_8_0004
//! doc: spec
//! section: 9.8
//! title: serialized optional on a non-defaultable choice component is rejected
//! rule: A serialized optional component whose type is not defaultable is a
//!   compile error at the conformance declaration; a missing field would have no
//!   value to supply.
//! cites: 17.11, 35.10
//! expect: reject
//! diagnostic: defaultable
module Conf;
import System.Serialization;

public choice Flag
  On,
  Off;
end Flag;

public record Setting: ISerializable, IDeserializable
  public readonly property Mode: Flag serialized optional;

  public constructor(mode: Flag)
    Mode := mode;
  end constructor;
end Setting;
