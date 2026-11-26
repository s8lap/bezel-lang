//! case: s9_8_3601
//! doc: spec
//! section: 9.8
//! title: the count BeginObject receives omits a null serialized optional component
//! rule: `s.BeginObject(n)` where `n` is the number of fields the call writes into this object — one for
//!   each `WriteFieldName` it makes there. That is the serialized components, less each `serialized
//!   optional` component omitted because it is null
//! cites: 9.8
//! expect: run
//! output: obj1;@A;int1;end;
//!   obj2;@A;int2;@N;str:z;end;
//! issue: 8090
//! notes: s9_8_0013's Tape, logging the count.
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
  public method BeginObject(fieldCount: int) => Log := Log + $"obj{fieldCount};";
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
