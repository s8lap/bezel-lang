for var p in Probes(8) do p.Run(); end;          // remedy 1: use each element in the borrowing body

using var kept := new OwningArray<Probe>();      // remedy 2: give the elements an owner
for var p in OwningProbes(8) do                  // OwningProbes(...): IOwningEnumerable<Probe>
  kept.Add(move p);
end;
var idle := kept.FirstOrNull(lambda(p) => not p.IsBusy());   // collection-backed ⇒ sound
