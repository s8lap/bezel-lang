//! case: s35_2_0001
//! doc: spec
//! section: 35.2
//! title: a choice body admits exactly what an enum body admits
//! rule: A choice body may declare, after the case list, exactly what an enum body may:
//!   instance methods, expression-bodied read-only instance properties, static
//!   methods, static readonly fields, const members, and operator overloads.
//! cites: 35.2, 10.4
//! expect: run
//! output: True
//!   high
//!   medium
//!   2
//!   priority
module Conf;

public choice Priority
  Low,
  Medium,
  High;

  public method IsUrgent(): bool => (case this of High => true, _ => false);

  public property Label: string => (case this of
    Low    => "low",
    Medium => "medium",
    High   => "high");

  public static method Default(): Priority => Medium;

  public const Max: int := 2;

  public static readonly Tag: string := "priority";
end Priority;

public class Program
  public static method Main()
    var p := Priority.High;
    Console.WriteLine(p.IsUrgent());          // instance method
    Console.WriteLine(p.Label);               // expression-bodied read-only property
    Console.WriteLine(Priority.Default().Label);  // static method
    Console.WriteLine(Priority.Max);          // const member
    Console.WriteLine(Priority.Tag);          // static readonly field
  end Main;
end Program;
