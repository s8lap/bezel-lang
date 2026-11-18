//! case: s14_2_6620
//! doc: spec
//! section: 14.2
//! title: the example: `where S: U` on `I<U>` is inherited as `where T: IFoo` at `I<IFoo>`
//! rule: the inherited set is read under the implemented interface's or overridden base's substitution, its method type parameters renamed positionally
//! cites: 14.2
//! expect: run
//! output: 4
//! issue: 8684
module Conf;
interface IFoo
  method Hi(): int;
end IFoo;
interface I<U>
  method M<S>(x: S): int where S: U;
end I;
class C: I<IFoo>
  public constructor()
  end constructor;
  public method M<T>(x: T): int where T: IFoo => x.Hi();
end C;
class Foo: IFoo
  public constructor()
  end constructor;
  public method Hi(): int => 4;
end Foo;
class P
  public static method Main(): int

    var c := new C();
    Console.WriteLine(c.M(new Foo()));
    return 0;
  end Main;
end P;
