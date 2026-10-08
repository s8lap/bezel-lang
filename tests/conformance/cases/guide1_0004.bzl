using var a := new Buffer();          // new T(...)            -> owned
var b := source.OpenBuffer();         // returns owned Buffer   -> owned
var c := source.Buffer;               // field / property read  -> borrowed (source: source)
var d := registry.Find("key");        // returns Buffer (no owned) -> borrowed
var t := source.PrefetchAsync();      // returns Task<Buffer>  -> owned (task calls mint)
var q := xs.Where(p);                 // returns a lazy query  -> owned (calls mint)
var parts := line.Split(':');         // returns owned Array<string> -> owned (trivial-dispose: no ceremony, §2)
var e := move holder.slot;            // move out of owned field -> owned
