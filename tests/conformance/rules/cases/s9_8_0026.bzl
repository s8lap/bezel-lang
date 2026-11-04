//! case: s9_8_0026
//! doc: spec
//! section: 9.8
//! title: an enum reads back by its serialized name, and an unmatched name raises
//! rule: The synthesized form writes the member's serialized name as a string
//!   (via `WriteString`) — its `serialized "<name>"` rename where one is
//!   declared, its identifier otherwise … — and reads it back by matching
//!   against the declared members' serialized names; … An encoded name
//!   matching no declared member raises `SerializationException` on read
//! cites: 9.8
//! expect: run
//! output: Low
//!   High
//!   SER
//! notes: deliberately does not read the renamed constant's identifier
//!   ("Low"): whether it is accepted on read is reported as a spec question
//!   (step 2's "every declared name maps to its value").
module Conf;

import System.Serialization;

public enum Level: ISerializable, IDeserializable
  Low serialized "lo", High;
end Level;

public class Program
  public static method Main()
    Show("\"lo\"");
    Show("\"High\"");
    Show("\"Mid\"");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json);
      var v := Level.Deserialize(d);
      Console.WriteLine((if v = Level.Low then "Low" else "High"));
    catch
      SerializationException => Console.WriteLine("SER")
    end;
  end Show;
end Program;
