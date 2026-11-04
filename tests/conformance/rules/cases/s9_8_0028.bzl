//! case: s9_8_0028
//! doc: spec
//! section: 9.8
//! title: a constructor requires clause failing during Deserialize raises SerializationException
//! rule: A contract clause evaluating `false` during the construction step — a
//!   `requires` or `ensures` clause of the selected constructor (§22.16) —
//!   raises `SerializationException` from the synthesized code, by the same
//!   implementation-internal route as the missing-, duplicate-, and
//!   mismatched-component faults above,
//! cites: 9.8, 22.16, 9.9
//! expect: run
//! output: ok 80
//!   SER
module Conf;

import System.Serialization;

public record Port: ISerializable, IDeserializable
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0)

    Value := value;
  end constructor;
end Port;

public class Program
  public static method Main()
    Show("{\"Value\": 80}");
    Show("{\"Value\": -1}");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json);
      var p := Port.Deserialize(d);
      Console.WriteLine($"ok {p.Value}");
    catch
      SerializationException => Console.WriteLine("SER"),
      ContractViolationException => Console.WriteLine("CVE")
    end;
  end Show;
end Program;
