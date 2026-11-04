//! case: s9_8_0032
//! doc: spec
//! section: 9.8
//! title: a serialized clause on a record that hand-writes both directions is rejected
//! rule: A `serialized` clause on a member of a type that declares no
//!   serialization conformance, or on a member of a type whose serialization is
//!   hand-written (synthesis suppressed — see below), is a compile error: the
//!   clause only has meaning as an input to synthesis,
//! cites: 9.8
//! expect: reject
//! diagnostic: serialized
//! notes: deleting the clause leaves l2_14_0003's shape, which is accepted.
module Conf;

public record Point(X: int serialized "x", Y: int): ISerializable, IDeserializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("x"); s.WriteInt(X);
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
