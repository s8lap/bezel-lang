//! case: s9_8_0049
//! doc: spec
//! section: 9.8
//! title: a camelCase constructor parameter binds its PascalCase component on Deserialize
//! rule: Where a synthesized `Deserialize` constructs by constructor call, it
//!   uses the *unique* constructor whose parameter list binds exactly the
//!   components to be bound, matched by name — order-insensitive and compared
//!   **case-insensitively**, so the conventional camelCase constructor
//!   parameter matches its PascalCase component
//! cites: 9.8
//! expect: run
//! stdout: 4
//! notes: twin of s9_8_0029 (one legal edit: the parameter `w` becomes `width`).
module Conf;

import System.Serialization;

public record Panel: ISerializable, IDeserializable
  public readonly property Width: int;

  public constructor(width: int)
    Width := width;
  end constructor;
end Panel;

public class Program
  public static method Main()
    using var d := new JsonDeserializer("{\"Width\": 4}");
    var p := Panel.Deserialize(d);
    Console.WriteLine(p.Width);
  end Main;
end Program;
