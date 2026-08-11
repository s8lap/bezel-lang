using var index := new Array<Connection>();      // borrowing
index.Add(primary);                              // primary stays owned by its owner
index.Clear();                                   // drops the reference; disposes nothing

using var pool := new OwningArray<Connection>(); // owning
pool.Add(Connect("primary"));                    // Connect returns owned — transfers in
pool.Add(Connect("backup"));
var c := pool[0];                                // indexer-get: a borrow of the slot
c.Ping();                                        // use it; you may not dispose it
// scope end: pool disposed → both connections disposed with it, no destructor written
