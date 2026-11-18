// Swift 5.x with Foundation. NOT COMPILED: no Swift toolchain on the research machine;
// written from memory.
// Swift: a throwing function is marked `throws`; every call is marked by the caller with one
// of three stances: `try` (pass it up or catch it), `try?` (turn it into nil) and `try!`
// (assert: a thrown error traps here). Bugs (index out of range, overflow, `!` on nil) trap.
import Foundation

// 1a. A file the user chose.
func openUserFile(_ url: URL) {
    do {
        let data = try Data(contentsOf: url)
        print("user-file: \(data.count) bytes")
    } catch let e as CocoaError where e.code == .fileReadNoSuchFile {
        print("user-file: no such file, pick another")
    } catch {
        print("user-file: cannot read: \(error)")
    }
}

// 1b. A file the app cannot run without: the same error, asserted at this call.
func openRequiredFile(_ url: URL) -> Data {
    try! Data(contentsOf: url)
}

// 2a. Optional telemetry: `try?` discards the error.
func sendTelemetry() async {
    _ = try? await URLSession.shared.data(from: URL(string: "http://127.0.0.1:1/metrics")!)
}

// 2b. The primary upstream: `try` passes the error to the request boundary.
func handleRequest() async throws -> Data {
    let (data, _) = try await URLSession.shared.data(from: URL(string: "http://127.0.0.1:1/api")!)
    return data
}

// 3a. A number the user typed: `Int(String)` does not throw; it returns an optional.
func parseUser(_ s: String) {
    guard let n = Int(s) else {
        print("user-number: not a number, ask again")
        return
    }
    print("user-number: \(n)")
}

// 3b. A constant the program wrote: `!` asserts the optional.
let port = Int("8080")!

// 4a. A user's key: the dictionary subscript returns an optional.   4b. The program's own key.
let limits = ["retries": 3]

func lookupUser(_ k: String) {
    if let v = limits[k] { print("user-key: \(v)") } else { print("user-key: unknown setting") }
}

func lookupOwn(_ k: String) -> Int { limits[k]! }
