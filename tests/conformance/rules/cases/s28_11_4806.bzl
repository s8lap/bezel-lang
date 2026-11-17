//! case: s28_11_4806
//! doc: spec
//! section: 28.11
//! title: a deserialized Array of records with a string? component keeps every element, 100 rounds
//! rule: The slot kind alone determines what every element operation does:
//! cites: 28.11, 9.8, 28.9
//! expect: run
//! stdout: 1800
//! issue: 8391
module Conf;

import System.Serialization;

public record E(Name: string, Alt: string?): ISerializable, IDeserializable;

public class Bag(id: int, owned items: Array<E>): ISerializable, IDeserializable
  public owned Items: Array<E> := items;
  public property Id: int => id;
end Bag;

public class Program
  public static method Main()
    var total := 0;
    for var k in 1..100 do
      using var b := new Bag(1, new Array<E>());
      using var s := new BinarySerializer();
      b.Serialize(s);
      using var bytes := s.ToBytes();
      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan());
      using var b2 := Bag.Deserialize(d);
      for var i in 1..3 do
        using var f := $"name{i},z".Split(',');
        b2.Items.Add(new E(f[0], f[1]));
      end;
      b2.Items.Add(new E("lit", null));
      for var r in b2.Items do
        total := total + r.Name.Length;
      end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
