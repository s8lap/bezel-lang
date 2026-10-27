//! case: s27_6_0029
//! doc: spec
//! section: 27.6
//! title: a method reference to ICollection's Contains is formed over an ICollection-typed receiver
//! rule: "Method references work for static methods, instance methods (the instance is captured at
//!   formation — by reference for class- or interface-typed receivers, **by copy** for value-shaped
//!   receivers ...)" (§27.6, v0.340). `ICollection<T>` declares `method Contains(item: T): bool`
//!   (stdlib §4.1), and §26.5 rule 1 commits instance resolution to the head's declared member, so
//!   `h.Contains` names that member and forms a bound `(int) -> bool` reference over the
//!   `ICollection<int>`-typed `h`. KNOWN DIVERGENCE (#4704): the compiler rejects the formation in
//!   codegen with the #853 fence (".Contains` through a polymorphic collection interface ... is not
//!   yet supported in codegen"), so the reference never forms; no spec sentence rejects it.
//! cites: 27.6, 26.5, 4.1
//! expect: accept
//! issue: 4704
module Conf;

public class Demo
  public method Go(): bool
    var a: Array<int> := [1, 2, 3];
    var h: ICollection<int> := a;
    var f: (int) -> bool := h.Contains;
    return f(2);
  end Go;
end Demo;
