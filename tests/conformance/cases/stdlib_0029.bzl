public class JsonSerializer: ISerializer
  public constructor();
  public constructor(options: JsonOptions);
  // ISerializer surface (§2.14) implemented for JSON text.
  public method ToBytes(): owned OwningBuffer<byte>;   // UTF-8 encoded result
  public override method ToString(): string;          // convenience: UTF-8 → string
end JsonSerializer;

public class JsonDeserializer: IDeserializer
  public constructor(input: ReadOnlySpan<byte>);         // UTF-8 JSON
  public constructor(input: ReadOnlyArraySpan<byte>);    // v0.165 (#1358): Array-world twin;
                                                          //   borrowed identically for the deserializer's lifetime
  public constructor(input: string);                  // convenience
  public constructor(input: ReadOnlySpan<byte>, options: JsonOptions);
  public constructor(input: ReadOnlyArraySpan<byte>, options: JsonOptions);  // v0.215: options parity
  public constructor(input: string, options: JsonOptions);                   //   for every input form
  // IDeserializer surface (§2.14) implemented for JSON text.
end JsonDeserializer;

public record JsonOptions(
  Indented: bool,                 // pretty-print (serializer); ignored on read
  IndentWidth: int,               // spaces per level when Indented; 2 when no options are passed
  AllowTrailingCommas: bool,      // deserializer leniency; false when no options are passed
  IgnoreUnknownFields: bool,      // deserializer: skip input fields that match
                                  //   no declared component instead of raising;
                                  //   false (strict) when no options are passed
  MaxDepth: int                   // nesting guard; 64 when no options are passed
);
// All five components are required: a positional record admits no default values
// (spec A.3 PositionalField), so `new JsonOptions(...)` supplies every one. The
// values above are what the no-options constructors behave as.

public class BinarySerializer: ISerializer
  public constructor();
  // ISerializer surface (§2.14) implemented for the compact binary format.
  public method ToBytes(): owned OwningBuffer<byte>;
end BinarySerializer;

public class BinaryDeserializer: IDeserializer
  public constructor(input: ReadOnlySpan<byte>);         // strict
  public constructor(input: ReadOnlyArraySpan<byte>);    // v0.165 (#1358): Array-world twin;
                                                          //   borrowed identically for the deserializer's lifetime
  public constructor(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool);
  // IDeserializer surface (§2.14) implemented for the compact binary format.
end BinaryDeserializer;

import System.Serialization;

public method SavePoint(p: Point): owned OwningBuffer<byte>
  using var s := new JsonSerializer();
  p.Serialize(s);                       // p implements ISerializable (§2.14)
  return s.ToBytes();                   // ownership of the buffer transfers out
end SavePoint;

public method LoadPoint(bytes: ReadOnlySpan<byte>): Point
  using var d := new JsonDeserializer(bytes);
  return Point.Deserialize(d);          // recognized static factory (§2.14)
end LoadPoint;
