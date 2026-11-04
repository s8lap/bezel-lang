//! case: s9_8_0013
//! doc: spec
//! section: 9.8
//! title: a null serialized optional component is omitted on write, a non-null one written
//! rule: **`serialized optional`** — read/write tolerance for that component.
//!   On write, if the component is a nullable `T?` whose value is null, the
//!   field is omitted (rather than written as `WriteNull`); otherwise it is
//!   written normally.
//! cites: 9.8
//! expect: run
//! output: obj;@A;int1;end;
//!   obj;@A;int2;@N;str:z;end;
//! notes: this Tape does not log BeginObject's count — whether `n` counts an
//!   omitted optional component is not settled (reported as a question).
module Conf;

public record Entry(A: int, N: string? serialized optional): ISerializable, IDeserializable;

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
  public method BeginObject(fieldCount: int) => Log := Log + "obj;";
  public method WriteFieldName(name: string) => Log := Log + $"@{name};";
  public method EndObject() => Log := Log + "end;";
  public method BeginArray(elementCount: int) => Log := Log + "arr;";
  public method EndArray() => Log := Log + "endarr;";
end Tape;

public class Program
  public static method Main()
    using var t := new Tape();
    var e := new Entry(1, null);
    e.Serialize(t);
    Console.WriteLine(t.Log);
    using var u := new Tape();
    var f := new Entry(2, "z");
    f.Serialize(u);
    Console.WriteLine(u.Log);
  end Main;
end Program;
