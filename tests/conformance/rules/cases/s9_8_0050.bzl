//! case: s9_8_0050
//! doc: spec
//! section: 9.8
//! title: a value-like choice declaring both serialization interfaces round-trips
//! rule: A choice is **serialization-eligible** when it is value-like (§35.4)
//!   and every payload field's type is admissible as a serialized component and
//!   record-admissible
//! cites: 9.8, 35.4
//! expect: run
//! stdout: held 3
//! notes: twin of s9_8_0045 (one legal edit: the payload `c: Cell` becomes
//!   `c: int`, making the choice value-like).
module Conf;

import System.Serialization;

public class Cell
  public constructor()
  end constructor;
end Cell;

public choice Slot: ISerializable, IDeserializable
  Held(c: int),
  Free;
end Slot;

public class Program
  public static method Main()
    var v := Slot.Held(3);
    using var s := new JsonSerializer();
    v.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    var back := Slot.Deserialize(d);
    Console.WriteLine((case back of Held(c) => $"held {c}", Free => "free"));
  end Main;
end Program;
