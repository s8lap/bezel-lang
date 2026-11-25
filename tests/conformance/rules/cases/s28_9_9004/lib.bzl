module Lib2;
public interface IHx
  property P: (IHx) -> int;
  method Drop();
end IHx;
public class Hx: IHx
  public property P: (IHx) -> int := lambda(x: IHx) => 0;
  public constructor() end constructor;
  public method Drop() P := lambda(x: IHx) => 0; end Drop;
  public method Fill(k: int) P := lambda(x: IHx) => do x.Drop(); return k + 1; end; end Fill;
end Hx;
