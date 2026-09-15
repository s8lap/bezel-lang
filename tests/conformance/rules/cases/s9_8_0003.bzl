//! case: s9_8_0003
//! doc: spec
//! section: 9.8
//! title: serialized skip on a class-typed component rescues serialization-eligibility
//! rule: serialized skip excludes the component from both directions entirely;
//!   a serialized skip component whose type is not defaultable is a compile
//!   error, so the remedy uses a nullable class-typed field. The explicit
//!   constructor still binds the skipped component: synthesized Deserialize
//!   step 2 matches the record's components — the serialized ones together
//!   with every `serialized skip` one, which binds its `default(T)`.
//! cites: 9.8, 17.11
//! expect: accept
module Conf;

public class Cache
  public constructor()
  end constructor;
end Cache;

public record Setting: ISerializable, IDeserializable
  public readonly property Mode: int;
  public readonly property CacheRef: Cache? serialized skip;

  public constructor(mode: int, cache: Cache?)
    Mode := mode;
    CacheRef := cache;
  end constructor;
end Setting;
