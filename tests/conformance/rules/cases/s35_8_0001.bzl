//! case: s35_8_0001
//! doc: spec
//! section: 35.8
//! title: a value-like choice boxes through interface conformance
//! rule: Value-like choices box per the general rule; the box is runtime-managed.
//! expect: run
//! stdout: active
module Conf;

public interface IDescribable
  method Describe(): string;
end IDescribable;

public choice Status: IDescribable
  Active,
  Inactive;

  public method Describe(): string => (case this of Active => "active", Inactive => "inactive");
end Status;

public class Program
  public static method Main()
    var s: IDescribable := Status.Active;   // boxing: value-like choice converts to an interface
    Console.WriteLine(s.Describe());
  end Main;
end Program;
