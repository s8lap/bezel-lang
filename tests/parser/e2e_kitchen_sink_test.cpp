// End-to-end "kitchen sink" parser test. The body is a single Bezel
// source file that exercises a broad selection of the constructs the
// parser supports so the test catches cross-construct regressions.
//
// Goal: this should not need to be updated every slice — only when a
// construct genuinely interacts badly with another (in which case it's
// the right surface to surface the conflict). The detailed shape
// assertions for individual constructs live in their own per-slice test
// files; here we just assert that the whole thing parses with zero
// diagnostics and produces the expected number of top-level decls.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

const char* kSample = R"BEZEL(
module Kitchen.Sink;

import System.Collections.Generic;
import System.Threading;

type Map<K, V> := IDictionary<K, V>;

public enum Color: int
  Red := 1 serialized "red",
  Green := 2 serialized "green",
  Blue := 3 serialized "blue";
  public method ToHex(): string => "#";
end Color;

public interface IRepository<T>
  method Save(item: T): bool;
  readonly property Count: int;
  event ItemSaved: T;
end IRepository;

public record Point(x: int serialized "X", y: int serialized "Y");

public class Service<T> :
  IDisposable
where T: class

  polymorphic("user": UserService, "admin": AdminService);
  private owned _store: IRepository<T>;
  private readonly _name: string;

  public property Name: string => _name;
  public property Endpoint: string private set := "localhost";
  public property[i: int]: T
    get => _store.Get(i);
    set => _store.Set(i, value);
  end;

  public const VERSION := "1.0";

  public constructor(name: string)
    this._name := name;
  end constructor;

  public method Add(owned item: T): bool
    return _store.Save(item);
  end Add;

  public async method LoadAll(): Task<()>
    var items := [1, 2, 3, 4, 5];
    var dict := ["a": 1, "b": 2];
    await using var src := source.OpenAsync();
    await for var x in src with token do
      _ := await ProcessAsync(x);
    end;
    for var i in 0..items.Count do
      var pt := default(Point);
      pt := pt with(x := i, y := i * 2);
      _ := pt;
    end;
  end LoadAll;

  public method Classify(x: int): string
    return (case x of
      0 => "zero",
      1 | 2 | 3 => "low",
      0..10 => "medium",
      var n when n > 0 => "positive",
      _ => "other"
    );
  end Classify;

  public method Process(items: [10]int): int
    var sum := 0;
    for var i in 0..10 step 1 do
      sum := sum + items[i];
    end;
    return sum;
  end Process;

  public method Sub(callback: (int) -> int)
    _ := callback(42);
  end Sub;

  public method ParallelArgs(name: string, ref count: int, owned r: Resource)
    Update(name := name, ref count, move r);
  end ParallelArgs;

  public method Dispose()
    try
      _ := _store.Save(default(T));
    catch IOException | NetworkException when ShouldRetry() =>
      retry();
    catch var e: Exception => do
      log(e);
      throw;
    end;
    finally
      cleanup();
    end;
  end Dispose;

  public static method operator =(a: Service<T>, b: Service<T>): bool
    => a._name = b._name;
end Service;

public extension string
  public method IsEmpty(): bool => this.Length = 0;
  public method Greet(name: string): string => $"Hello, {name}!";
end;

extern "libnet" convention "cdecl"
  method socket_open(host: string, port: int): int;
end;
)BEZEL";

} // namespace

BEZEL_TEST(parser_e2e, kitchen_sink_parses_cleanly) {
    auto r = parseAll(kSample);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    if (r.sourceFile) {
        // 6 top-level decls: type alias, enum, interface, record, class,
        // extension, extern block — actually 7.
        BEZEL_EXPECT_EQ(
            r.sourceFile->topDecls.size(), std::size_t(7));
    }
}
