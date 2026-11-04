//! case: l2_15_0035
//! doc: stdlib
//! section: 2.15
//! title: the agent path reads a multi-value stream one value after another
//! rule: the agent path deliberately leaves remaining input unread and never faults
//!   on it — the cursor may rest mid-stream for a subsequent read, which is
//!   exactly how a multi-value stream is read
//! cites: 2.15
//! expect: run
//! output: OK 1 2 3 4
//!   OK 1 2 3 4
//! notes: Each Deserialize call leaves the cursor after the value it read, so the
//!   next call reads the next value; the second stream makes that hold across
//!   values that contain arrays.
module Conf;

import System.Serialization;

record RT(V: int, W: int): ISerializable, IDeserializable;
record RA(V: [2]int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(M1("{\"V\":1,\"W\":2} {\"V\":3,\"W\":4}"));
    Console.WriteLine(M2("{\"V\":[1,2]} {\"V\":[3,4]}"));
  end Main;

  private static method M1(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var a := RT.Deserialize(d);
      var b := RT.Deserialize(d);
      r := $"OK {a.V} {a.W} {b.V} {b.W}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end M1;

  private static method M2(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var a := RA.Deserialize(d);
      var b := RA.Deserialize(d);
      r := $"OK {a.V[0]} {a.V[1]} {b.V[0]} {b.V[1]}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end M2;
end Program;
