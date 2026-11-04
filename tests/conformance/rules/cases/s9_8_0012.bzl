//! case: s9_8_0012
//! doc: spec
//! section: 9.8
//! title: a null nullable component without optional is written as WriteNull
//! rule: For each component in declaration order: `s.WriteFieldName("<name>")`
//!   followed by the component's encoding — … `WriteNull` for a null nullable.
//! cites: 9.8
//! expect: run
//! output: A=1 N True
//!   A=2 N False z
module Conf;

import System.Serialization;

public record Entry(A: int, N: string?): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show(new Entry(1, null));
    Show(new Entry(2, "z"));
  end Main;

  private static method Show(e: Entry)
    using var s := new JsonSerializer();
    e.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    _ := d.BeginObject();
    var n1 := d.ReadFieldName();
    var a := d.ReadInt();
    var n2 := d.ReadFieldName();
    var isNull := d.TryReadNull();
    if isNull then
      Console.WriteLine($"{n1}={a} {n2} {isNull}");
    else
      Console.WriteLine($"{n1}={a} {n2} {isNull} {d.ReadString()}");
    end;
  end Show;
end Program;
