//! case: l2_15_9104
//! doc: stdlib
//! section: 2.15
//! title: malformed JSON arrays of choices raise SerializationException and a well-formed one reads
//! rule: **Errors.** Malformed input, type mismatches
//! cites: 2.15, 9.8
//! expect: run
//! output: SER
//!   SER
//!   OK 2
//! issue: 9048
//! notes: Pins #9048 (pick 026db0dab): the JSON choice reader did not take the element separator,
//!   so an array of value-like choices serialized but failed to deserialize past its first
//!   element. Rows 1 and 2 are the negative direction (JSON requires a ',' between elements
//!   and forbids a leading one; stdlib 2.15 makes malformed input a SerializationException)
//!   and must stay rejected; row 3 is the well-formed twin and is the row the base got wrong
//!   (it raised at the second element).
module Conf;

import System.Serialization;

public choice Msg: ISerializable, IDeserializable
  Num(n: long),
  Empty;
end Msg;

public class Program
  static method Try(json: string)
    try
      using var d := new JsonDeserializer(json);
      _ := d.BeginArray();
      var n := 0;
      while d.HasMoreElements() do
        var m := Msg.Deserialize(d);
        n++;
      end;
      d.EndArray();
      Console.WriteLine("OK " + n.ToString());
    catch var ex: SerializationException =>
      Console.WriteLine("SER");
    end;
  end Try;

  public static method Main(): int
    Try("[{\"$case\":\"Num\",\"n\":1} {\"$case\":\"Num\",\"n\":2}]");
    Try("[,{\"$case\":\"Num\",\"n\":1}]");
    Try("[{\"$case\":\"Num\",\"n\":1},{\"$case\":\"Empty\"}]");
    return 0;
  end Main;
end Program;
