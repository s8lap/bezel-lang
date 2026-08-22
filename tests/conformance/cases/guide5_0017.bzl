using var probes := Probes(8);    // Probes is an iterator minting fresh Probe objects
var p := probes.First();          // borrow of the element — compiles, but WARNS
                                  //   (stdlib §5, spec §29.11: same-unit
                                  //    enumerator-owned-yield root)
p.Run();                          // checked mode: DisposedAccessException — the element
                                  //   died with First()'s internal enumerator
