//! case: s28_11_4807
//! doc: spec
//! section: 28.11
//! title: a deserialized Array<string> keeps strings added from Split temporaries, 100 rounds
//! rule: The slot kind alone determines what every element operation does:
//! cites: 28.11, 9.8, 28.9
//! expect: run
//! stdout: 1700
//! issue: 8391
module Conf;

import System.Serialization;

public class Bag(id: int, owned items: Array<string>): ISerializable, IDeserializable
  public owned Items: Array<string> := items;
  public property Id: int => id;
end Bag;

public class Program
  public static method Main()
    var total := 0;
    for var k in 1..100 do
      using var b := new Bag(1, new Array<string>());
      b.Items.Add("x0");
      using var s := new BinarySerializer();
      b.Serialize(s);
      using var bytes := s.ToBytes();
      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan());
      using var b2 := Bag.Deserialize(d);
      for var i in 1..3 do
        using var f := $"name{i},z".Split(',');
        b2.Items.Add(f[0]);
      end;
      for var r in b2.Items do
        total := total + r.Length;
      end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
