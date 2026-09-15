//! case: l2_14_0006
//! doc: stdlib
//! section: 2.14
//! title: a class target's Deserialize without owned is rejected
//! rule: The owned modifier appears exactly when the declaring type is a
//!   class; a class's recognized Deserialize factory that omits owned is
//!   rejected — a plain class-typed return reads as a borrow, and a static
//!   method's borrow-shaped return of a fresh value is rejected.
//! cites: 2.14, 28.3, 28.8
//! expect: reject
//! diagnostic: owned
module Conf;

public class Widget: ISerializable, IDeserializable
  public constructor()
  end constructor;

  public method Serialize(s: ISerializer)
    s.BeginObject(0);
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): Widget
    _ := d.BeginObject();
    d.EndObject();
    return new Widget();
  end Deserialize;
end Widget;
