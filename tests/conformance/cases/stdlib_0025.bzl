import System.Serialization;

public method SavePoint(p: Point): owned OwnedBuffer<byte>
  using var s := new JsonSerializer();
  p.Serialize(s);                       // p implements ISerializable (§2.14)
  return s.ToBytes();                   // ownership of the buffer transfers out
end SavePoint;

public method LoadPoint(bytes: ReadOnlySpan<byte>): Point
  using var d := new JsonDeserializer(bytes);
  return Point.Deserialize(d);          // recognized static factory (§2.14)
end LoadPoint;
