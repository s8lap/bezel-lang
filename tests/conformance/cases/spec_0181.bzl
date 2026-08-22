// _pool is a field of the enclosing class: private owned _pool: OwningArray<Connection>;
var adopt: (owned Connection) -> () := lambda(owned c) => _pool.Add(move c);
