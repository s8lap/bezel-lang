//! case: s9_3_0005
//! doc: spec
//! section: 9.3
//! title: a positional field marked equality skip is excluded from equality but still constructed and rendered
//! rule: A positional field may carry equality skip; the field is excluded from
//!   equality but present, constructed, and rendered, and a skipped field's
//!   inadmissible type does not disqualify the record.
//! cites: 9.2 (equality skip clause)
//! expect: run
//! stdout: True,cpu=10
module Conf;

public record Metric(Name: string, Value: int equality skip)
  public method Describe(): string => $"{Name}={Value}";
end Metric;

public class Program
  public static method Main()
    var a := new Metric("cpu", 10);
    var b := new Metric("cpu", 99);
    Console.WriteLine($"{a = b},{a.Describe()}");
  end Main;
end Program;
