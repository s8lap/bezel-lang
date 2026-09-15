//! case: l2_14_0004
//! doc: stdlib
//! section: 2.14
//! title: owned on a value-type target's Deserialize return is rejected
//! rule: owned on a record, enum, or choice return is a compile error
//!   (language spec §28.1's type requirement); the recognized factory for a
//!   value-type target must not spell owned.
//! cites: 2.14, 28.1
//! expect: reject
//! diagnostic: owned
module Conf;

public record Point(X: int, Y: int): ISerializable, IDeserializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("X"); s.WriteInt(X);
    s.WriteFieldName("Y"); s.WriteInt(Y);
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): owned Point
    _ := d.BeginObject();
    var x := 0;  var y := 0;
    d.EndObject();
    return new Point(x, y);
  end Deserialize;
end Point;
