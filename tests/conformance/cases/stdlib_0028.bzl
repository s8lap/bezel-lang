// One-shot facades over the agent pairs. The write members are ordinary
// constrained generics; Parse/ParseOwned are compiler-known (see below).
public static class Json
  // Write: value → encoded form. `value` is borrowed for the call; the
  // options overloads thread to the JsonSerializer constructor.
  public static method ToText<T>(value: T): string where T: ISerializable;
  public static method ToText<T>(value: T, options: JsonOptions): string where T: ISerializable;
  public static method ToBytes<T>(value: T): owned OwningBuffer<byte> where T: ISerializable;
  public static method ToBytes<T>(value: T, options: JsonOptions): owned OwningBuffer<byte> where T: ISerializable;

  // Read: encoded form → value, for value-type targets (records, enums,
  // value-like choices — spec §9.8); a class-typed T is a compile-time error
  // directing to ParseOwned<T>. The input forms mirror the JsonDeserializer
  // constructor set above one-for-one.
  public static method Parse<T>(text: string): T where T: IDeserializable;
  public static method Parse<T>(text: string, options: JsonOptions): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlySpan<byte>): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlySpan<byte>, options: JsonOptions): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>, options: JsonOptions): T where T: IDeserializable;

  // Read, class targets: the recognized factory's owned T transfers to the
  // caller. Same six input forms as Parse<T>.
  public static method ParseOwned<T>(text: string): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(text: string, options: JsonOptions): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlySpan<byte>): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlySpan<byte>, options: JsonOptions): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>, options: JsonOptions): owned T where T: class, IDeserializable;
end Json;

public static class Binary
  // The binary format is bytes-only: no text form. It has no options record;
  // its one knob is the BinaryDeserializer constructor bool, mirrored here.
  public static method ToBytes<T>(value: T): owned OwningBuffer<byte> where T: ISerializable;

  public static method Parse<T>(input: ReadOnlySpan<byte>): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>): T where T: IDeserializable;
      // no (ReadOnlyArraySpan<byte>, ignoreUnknownFields) form: the facade
      // mirrors the BinaryDeserializer constructor set above, which declares
      // no such constructor

  public static method ParseOwned<T>(input: ReadOnlySpan<byte>): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>): owned T where T: class, IDeserializable;
end Binary;

import System.Serialization;
import System.IO;

// The SavePoint/LoadPoint pair, restated through the facade (Point: the §2.14
// example record). The agent-driven bodies below are what these calls mean.
public method SavePoint(p: Point): owned OwningBuffer<byte>
  return Json.ToBytes(p);
end SavePoint;

public method LoadPoint(bytes: ReadOnlySpan<byte>): Point
  return Json.Parse<Point>(bytes);
end LoadPoint;

// A class target arrives owned (Order: the §2.14 example class); binary bytes
// compose with the §7.3 statics — WriteAllBytes' span overload is the
// zero-copy serializer-output path.
public method Reencode(text: string, path: string)
  using var order := Json.ParseOwned<Order>(text);
  using var blob  := Binary.ToBytes(order);
  File.WriteAllBytes(path, blob.AsReadOnlySpan());
end Reencode;
