var x: Buffer;
if useFresh then
  x := new Buffer();              // owned
else
  x := source.CloneBuffer();      // returns owned Buffer — also owned
end;
// x is owned on both branches — the merge is fine.
// An owned var still carries the disposal obligation; discharge it by
// binding into a using var, returning it under an owned return type,
// or storing it into an owned field:
using var owned_x := x;           // adopt for scoped disposal
owned_x.Process();
