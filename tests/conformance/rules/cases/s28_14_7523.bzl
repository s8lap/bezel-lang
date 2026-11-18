//! case: s28_14_7523
//! doc: spec
//! section: 28.14
//! title: this passed as a scoped borrow to a method of a parameter receiver is rejected
//! rule: 5. otherwise → the receiver.
//! cites: 28.14
//! expect: reject
//! diagnostic: scoped
module Conf;
// METHOD form: `this` passed as a scoped borrow to a method on a PARAMETER receiver (form 5): the registry outlives the owner.
public class Registry
  private _o: Owner?;
  public constructor()
    _o := null;
  end constructor;
  public method Attach(scoped o: Owner)
    _o := o;
  end Attach;
  public method Name(): string => _o!.Name;
end Registry;
public class Owner
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
  public method Join(r: Registry)
    r.Attach(scoped this);
  end Join;
end Owner;
public class Program
  public static method Main(): int
    using var reg := new Registry();
    do
      using var o := new Owner("temp");
      o.Join(reg);
    end;
    Console.WriteLine(reg.Name());
    return 0;
  end Main;
end Program;
