//! case: s9_8_0048
//! doc: spec
//! section: 9.8
//! title: serialized skip on a nullable string component is admitted and reads back null
//! rule: A `serialized skip` component whose type is not defaultable (§17.11) is
//!   a compile error at the conformance declaration (the synthesized
//!   `Deserialize` would have no value to construct with).
//! cites: 9.8, 17.11
//! expect: run
//! stdout: 1 True
//! notes: twin of s9_8_0030 (one legal edit: `string` becomes `string?`, whose
//!   `default` is null).
module Conf;

import System.Serialization;

public record Login(Id: int, Token: string? serialized skip): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var l := new Login(1, "secret");
    using var s := new JsonSerializer();
    l.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    var back := Login.Deserialize(d);
    Console.WriteLine($"{back.Id} {back.Token = null}");
  end Main;
end Program;
