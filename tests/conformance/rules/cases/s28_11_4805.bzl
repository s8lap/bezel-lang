//! case: s28_11_4805
//! doc: spec
//! section: 28.11
//! title: elements added to a deserialized Array of records read back intact after the Split temporaries die
//! rule: The slot kind alone determines what every element operation does:
//! cites: 28.11, 9.8, 28.9
//! expect: run
//! output: name1
//!   name2
//!   name3
//! issue: 8391
module Conf;

import System.Serialization;

public record E(Name: string, N: int): ISerializable, IDeserializable;

public class Bag(id: int, owned items: Array<E>): ISerializable, IDeserializable
  public owned Items: Array<E> := items;
  public property Id: int => id;
end Bag;

public class Program
  public static method Main()
    using var b := new Bag(1, new Array<E>());
    using var s := new BinarySerializer();
    b.Serialize(s);
    using var bytes := s.ToBytes();
    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan());
    using var b2 := Bag.Deserialize(d);
    for var i in 1..3 do
      using var f := $"name{i},z".Split(',');
      b2.Items.Add(new E(f[0], i));
    end;
    var junk := new Array<string>();
    for var i in 1..100 do
      junk.Add($"junkjunkjunk{i}");
    end;
    for var r in b2.Items do
      Console.WriteLine(r.Name);
    end;
  end Main;
end Program;
