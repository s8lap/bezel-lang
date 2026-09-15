//! case: l2_14_0003
//! doc: stdlib
//! section: 2.14
//! title: a hand-written Deserialize for a value-type target returns plain T
//! rule: For a value-type target (a record, enum, or value-like choice) the
//!   recognized Deserialize factory returns a plain by-value T with no
//!   owned modifier.
//! cites: 2.14, 28.1
//! expect: accept
module Conf;

public record Point(X: int, Y: int): ISerializable, IDeserializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("X"); s.WriteInt(X);
    s.WriteFieldName("Y"); s.WriteInt(Y);
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): Point
    _ := d.BeginObject();
    var x := 0;  var y := 0;
    d.EndObject();
    return new Point(x, y);
  end Deserialize;
end Point;
