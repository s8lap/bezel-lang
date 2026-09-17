//! case: l2_15_0002
//! doc: stdlib
//! section: 2.15
//! title: Json.Parse<T> rejects a class-typed T at compile time
//! rule: Parse<T> requires an IDeserializable-conforming value-type target
//!   and rejects a class-typed T at compile time, directing to
//!   ParseOwned<T>.
//! cites: 2.15
//! expect: reject
//! diagnostic: ParseOwned
module Conf;

import System.Serialization;

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

public class Program
  public method Bad(text: string)
    _ := Json.Parse<Widget>(text);
  end Bad;
end Program;
