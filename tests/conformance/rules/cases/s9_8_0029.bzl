//! case: s9_8_0029
//! doc: spec
//! section: 9.8
//! title: a deserializable record whose only constructor binds no component by name is rejected
//! rule: If no constructor matches, more than one does, or the case-insensitive
//!   comparison leaves a parameter ambiguous between two components, the
//!   conformance declaration is a compile error naming the candidate
//!   constructors —
//! cites: 9.8
//! expect: reject
//! diagnostic: constructor
//! notes: twin s9_8_0001 — `constructor(name: string, value: int)` binds
//!   `Name`/`Value` case-insensitively and is accepted.
module Conf;

public record Panel: ISerializable, IDeserializable
  public readonly property Width: int;

  public constructor(w: int)
    Width := w;
  end constructor;
end Panel;
