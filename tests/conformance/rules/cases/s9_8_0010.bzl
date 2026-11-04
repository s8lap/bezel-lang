//! case: s9_8_0010
//! doc: spec
//! section: 9.8
//! title: a serialized skip component is not written and is not counted in BeginObject
//! rule: **`serialized skip`** — the component is excluded from both directions
//!   entirely. The synthesized `Serialize` does not write it; …
//! cites: 9.8
//! expect: run
//! stdout: obj1;@A;int1;end;
//! notes: the count follows step 1, `n` being the number of serialized
//!   components; a skipped component is not one.
module Conf;

public record Pair(A: int, B: int serialized skip): ISerializable, IDeserializable;

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
    var p := new Pair(1, 9);
    p.Serialize(t);
    Console.WriteLine(t.Log);
  end Main;
end Program;
