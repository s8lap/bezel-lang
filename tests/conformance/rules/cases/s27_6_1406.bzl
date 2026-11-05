//! case: s27_6_1406
//! doc: spec
//! section: 27.6
//! title: a bound reference over a string field captures the field's value at formation under both spellings
//! rule: the reference is a bound reference: the receiver is captured at formation as the
//!   instance-method sentence above states (by reference for a class- or interface-typed
//!   receiver, by copy for a value-shaped one — a record, an enum, a copyable choice, a
//!   non-owning inline array, a primitive, the unit value, a `string` or a function value
//! cites: 27.6
//! expect: run
//! stdout: ann!,ann!,bob!
module Conf;

extension string
  public method Shout(): string => this + "!";
end extension;

public class P
  public name: string;

  public constructor(n: string)
    this.name := n;
  end constructor;

  public method Mk(): () -> string => this.name.Shout;
  public method Mk2(): () -> string => name.Shout;
end P;

public class Program
  public static method Main()
    using var p := new P("ann");
    var f := p.Mk();
    var g := p.Mk2();
    p.name := "bob";
    var h := p.Mk();
    Console.WriteLine(f() + "," + g() + "," + h());
  end Main;
end Program;
