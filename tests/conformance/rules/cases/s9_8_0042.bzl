//! case: s9_8_0042
//! doc: spec
//! section: 9.8
//! title: an equality-skipped explicit field leaves a body record serialization-eligible
//! rule: the one shared reading is this section's eligibility gate, which
//!   reads both clauses (so a record with an equality-skipped explicit cache
//!   field stays serialization-eligible with no second clause: explicit fields
//!   were never serialized components).
//! cites: 9.8, 9.2
//! expect: run
//! stdout: Lo=1 Hi=5
//! notes: twin of s9_8_0041; the field is not written (never a serialized
//!   component).
module Conf;

import System.Serialization;

public record Interval: ISerializable, IDeserializable
  public readonly property Lo: int;
  public readonly property Hi: int;
  private equality skip _cachedLen: int;

  public constructor(lo: int, hi: int)
    Lo := lo;
    Hi := hi;
    _cachedLen := hi - lo;
  end constructor;
end Interval;

public class Program
  public static method Main()
    var r := new Interval(1, 5);
    using var s := new JsonSerializer();
    r.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    _ := d.BeginObject();
    var n1 := d.ReadFieldName();
    var v1 := d.ReadInt();
    var n2 := d.ReadFieldName();
    var v2 := d.ReadInt();
    d.EndObject();
    Console.WriteLine($"{n1}={v1} {n2}={v2}");
  end Main;
end Program;
