//! case: s9_8_0009
//! doc: spec
//! section: 9.8
//! title: a positional record's synthesized Serialize writes a counted object in declaration order
//! rule: For an eligible record, the synthesized `Serialize(s: ISerializer)`
//!   body is: 1. `s.BeginObject(n)` where `n` is the number of serialized
//!   components. 2. For each component in declaration order:
//!   `s.WriteFieldName("<name>")` followed by the component's encoding —
//!   `s.WriteInt`/`WriteString`/etc. for a primitive or `string`; … 3.
//!   `s.EndObject()`.
//! cites: 9.8, 9.1
//! expect: run
//! stdout: obj2;@B;int7;@A;str:x;end;
//! notes: Tape is a recording ISerializer (stdlib 2.14's interface) so the
//!   exact token sequence is observed independently of any format.
module Conf;

public record Pair(B: int, A: string): ISerializable, IDeserializable;

public class Tape: ISerializer
  public Log: string := "";

  public method WriteInt(value: long) => Log := Log + $"int{value};";
  public method WriteUInt(value: ulong) => Log := Log + $"uint{value};";
  public method WriteFloat(value: double) => Log := Log + $"float{value};";
  public method WriteDecimal(value: decimal) => Log := Log + $"dec{value};";
  public method WriteBool(value: bool) => Log := Log + $"bool{value};";
  public method WriteString(value: string) => Log := Log + $"str:{value};";
  public method WriteChar(value: char) => Log := Log + $"char:{value};";
  public method WriteNull() => Log := Log + "null;";
  public method BeginObject(fieldCount: int) => Log := Log + $"obj{fieldCount};";
  public method WriteFieldName(name: string) => Log := Log + $"@{name};";
  public method EndObject() => Log := Log + "end;";
  public method BeginArray(elementCount: int) => Log := Log + "arr;";
  public method EndArray() => Log := Log + "endarr;";
end Tape;

public class Program
  public static method Main()
    using var t := new Tape();
    var p := new Pair(7, "x");
    p.Serialize(t);
    Console.WriteLine(t.Log);
  end Main;
end Program;
