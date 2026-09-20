// Agent: writes primitive and structural tokens in a concrete format.
// Implemented by JsonSerializer, BinarySerializer, etc.
public interface ISerializer
  method WriteInt(value: long);
  method WriteUInt(value: ulong);
  method WriteFloat(value: double);
  method WriteDecimal(value: decimal);
  method WriteBool(value: bool);
  method WriteString(value: string);
  method WriteChar(value: char);
  method WriteNull();
  method BeginObject(fieldCount: int);
  method WriteFieldName(name: string);
  method EndObject();
  method BeginArray(elementCount: int);
  method EndArray();
end ISerializer;

// Agent: reads the same tokens back. Mirror of ISerializer.
public interface IDeserializer
  method ReadInt(): long;
  method ReadUInt(): ulong;
  method ReadFloat(): double;
  method ReadDecimal(): decimal;
  method ReadBool(): bool;
  method ReadString(): string;
  method ReadChar(): char;
  method TryReadNull(): bool;            // true and consumes if next is null
  method BeginObject(): int;             // returns field count if the format
                                         //   provides one, else -1
  method ReadFieldName(): string;
  method SkipValue();                    // consume and discard the current
                                         //   value (including nested object/
                                         //   array structure); used by
                                         //   synthesized Deserialize for
                                         //   unknown fields. A strict
                                         //   deserializer raises on the
                                         //   unknown field before this is
                                         //   reached; a tolerant one skips.
  method EndObject();
  method BeginArray(): int;              // element count, or -1 if unknown
  method HasMoreElements(): bool;        // for count = -1 streaming formats
  method EndArray();
end IDeserializer;

// Capability: this type can write itself to any ISerializer.
public interface ISerializable
  method Serialize(s: ISerializer);
end ISerializable;

// Capability: this type can be reconstructed from any IDeserializer.
// The reconstruction entry point is a static factory; see the note below
// on why this is recognized by name rather than declared as an interface
// method in this version.
public interface IDeserializable
end IDeserializable;

public record Point(X: int, Y: int): ISerializable, IDeserializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("X"); s.WriteInt(X);
    s.WriteFieldName("Y"); s.WriteInt(Y);
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): Point
    _ := d.BeginObject();   // field count, or -1 for JSON; unused here
    var x := 0;  var y := 0;
    // field-name dispatch omitted for brevity; a real implementation
    // loops over ReadFieldName() and fills the locals
    d.EndObject();
    return new Point(x, y);
  end Deserialize;
end Point;
