//! case: s9_8_0023
//! doc: spec
//! section: 9.8
//! title: a renamed component is read under its serialized name, not its identifier
//! rule: **`serialized "<name>"`** — the component is serialized and
//!   deserialized under `<name>` instead of its declared identifier.
//! cites: 9.8
//! expect: run
//! output: ok 5
//!   SER
//! notes: the second input lacks "alpha", a missing required component,
//!   which raises whatever the unknown-field policy.
module Conf;

import System.Serialization;

public record Wrap(A: int serialized "alpha"): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"alpha\": 5}");
    Show("{\"A\": 5}");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json);
      var w := Wrap.Deserialize(d);
      Console.WriteLine($"ok {w.A}");
    catch
      SerializationException => Console.WriteLine("SER")
    end;
  end Show;
end Program;
