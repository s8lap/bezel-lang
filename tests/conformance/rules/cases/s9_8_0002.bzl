//! case: s9_8_0002
//! doc: spec
//! section: 9.8
//! title: a record with a class-typed serialized component is not serialization-eligible
//! rule: A record's serialized components must be record-admissible; a record
//!   with a class-typed serialized component is not serialization-eligible, a
//!   compile error at the conformance declaration naming the field.
//! expect: reject
//! diagnostic: record-admissible
module Conf;
import System.Serialization;

public class Cache
  public constructor()
  end constructor;
end Cache;

public record Setting: ISerializable, IDeserializable
  public readonly property Mode: int;
  public readonly property CacheRef: Cache;

  public constructor(mode: int, cache: Cache)
    Mode := mode;
    CacheRef := cache;
  end constructor;
end Setting;
