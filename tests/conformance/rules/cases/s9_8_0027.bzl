//! case: s9_8_0027
//! doc: spec
//! section: 9.8
//! title: a record with an explicit constructor is deserialized through that constructor
//! rule: A record that declares one or more **explicit constructors** is
//!   constructed by *Constructor selection* (above): … Where a synthesized
//!   `Deserialize` constructs by constructor call, it uses the *unique*
//!   constructor whose parameter list binds exactly the components to be
//!   bound, matched by name — order-insensitive and compared
//!   **case-insensitively**,
//! cites: 9.8, 9.2
//! expect: run
//! stdout: 6
//! notes: the constructor normalizes, so a storage-level construction (the
//!   no-explicit-constructor path) would print 3.
module Conf;

import System.Serialization;

public record Box: ISerializable, IDeserializable
  public readonly property Size: int;

  public constructor(size: int)
    Size := size * 2;
  end constructor;
end Box;

public class Program
  public static method Main()
    using var d := new JsonDeserializer("{\"Size\": 3}");
    var b := Box.Deserialize(d);
    Console.WriteLine(b.Size);
  end Main;
end Program;
