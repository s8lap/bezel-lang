public class JsonSerializer: ISerializer, IDisposable
  public constructor();
  public constructor(options: JsonOptions);
  // ISerializer surface (§2.14) implemented for JSON text.
  public method ToBytes(): owned OwningBuffer<byte>;   // UTF-8 encoded result
  public method ToString(): string;                   // convenience: UTF-8 → string
end JsonSerializer;

public class JsonDeserializer: IDeserializer, IDisposable
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
  IndentWidth: int,               // spaces per level when Indented; default 2
  AllowTrailingCommas: bool,      // deserializer leniency; default false
  IgnoreUnknownFields: bool,      // deserializer: skip input fields that match
                                  //   no declared component instead of raising;
                                  //   default false (strict)
  MaxDepth: int                   // nesting guard; default 64
);

public class BinarySerializer: ISerializer, IDisposable
  public constructor();
  // ISerializer surface (§2.14) implemented for the compact binary format.
  public method ToBytes(): owned OwningBuffer<byte>;
end BinarySerializer;

public class BinaryDeserializer: IDeserializer, IDisposable
  public constructor(input: ReadOnlySpan<byte>);         // strict
  public constructor(input: ReadOnlyArraySpan<byte>);    // v0.165 (#1358): Array-world twin;
                                                          //   borrowed identically for the deserializer's lifetime
  public constructor(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool);
  // IDeserializer surface (§2.14) implemented for the compact binary format.
end BinaryDeserializer;
