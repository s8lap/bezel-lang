using var probes := Probes(8);     // Probes: an iterator minting fresh Probe objects
var p := probes.First();           // compiles, but WARNS — a dead borrow
var arr := probes.ToArray();       // NOT a remedy: the slots borrow the same dead elements
