var r      := header.AsBitReader();
var id     := ushort(r.ReadBitsBigEndianMsbFirst(16));  r := r.Slice(16);
var qr     := r.ReadBitMsbFirst();                      r := r.Slice(1);
var opcode := byte(r.ReadBitsBigEndianMsbFirst(4));     r := r.Slice(4);
