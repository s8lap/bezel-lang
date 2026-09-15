//! case: l2_14_0005
//! doc: stdlib
//! section: 2.14
//! title: a hand-written Deserialize for a class target returns owned T
//! rule: The owned modifier appears exactly when the declaring type is a
//!   class — a class instance is an owned heap value transferred to the
//!   caller.
//! cites: 2.14, 28.8
//! expect: accept
module Conf;

public class Widget: ISerializable, IDeserializable
  public constructor()
  end constructor;

  public method Serialize(s: ISerializer)
    s.BeginObject(0);
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): owned Widget
    _ := d.BeginObject();
    d.EndObject();
    return new Widget();
  end Deserialize;
end Widget;
