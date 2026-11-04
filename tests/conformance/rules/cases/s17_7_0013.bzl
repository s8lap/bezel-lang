//! case: s17_7_0013
//! doc: spec
//! section: 17.7
//! title: BinaryDeserializer with a third argument is rejected: two parameters at most
//! rule: `new T(...)` resolves only against a declared constructor, never against a
//!   compiler-supplied fallback
//! cites: 17.7, 22.5
//! expect: reject
//! diagnostic: constructor
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    using var w := new BinarySerializer();
    w.WriteInt(7);
    using var bytes := w.ToBytes();
    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(), true, true);
  end Main;
end Program;
