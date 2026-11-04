//! case: l2_15_0030
//! doc: stdlib
//! section: 2.15
//! title: AllowTrailingCommas decides whether an array's trailing comma is read
//! rule: AllowTrailingCommas: bool, // deserializer leniency; false when no options
//!   are passed
//! cites: 2.15
//! expect: run
//! output: SER
//!   OK 1 2
//!   OK 1 2
//!   SER
//! notes: The option is the stdlib's only statement of trailing-comma tolerance:
//!   false (the default) leaves the comma malformed, true tolerates it. The
//!   fourth input keeps a missing comma between the elements, which the option
//!   does not excuse.
module Conf;

import System.Serialization;

record RA(V: [2]int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Opt("{\"V\": [1, 2,]}", false));
    Console.WriteLine(Opt("{\"V\": [1, 2,]}", true));
    Console.WriteLine(Opt("{\"V\": [1, 2 ,]}", true));
    Console.WriteLine(Opt("{\"V\": [1 2,]}", true));
  end Main;

  private static method Opt(j: string, allow: bool): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j, new JsonOptions(false, 2, allow, false, 64));
      var v := RA.Deserialize(d);
      r := $"OK {v.V[0]} {v.V[1]}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end Opt;
end Program;
