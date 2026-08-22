var b := item.Handle();            // borrow of item
pool.Add(move item);               // ERROR: b is still live (the consume-site freeze)
b.Configure();
