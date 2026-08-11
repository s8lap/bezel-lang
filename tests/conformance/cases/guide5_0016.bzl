using var fleet := new OwningArray<Connection>();
fleet.Add(Connect("a"));
fleet.Add(Connect("b"));

var idle := fleet.FirstOrNull(lambda(c) => not c.IsBusy());  // borrow of the element
idle?.Ping();                     // fine while fleet lives — fleet is the backing
