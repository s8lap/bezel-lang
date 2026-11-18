// Kotlin 1.9 on the JVM. NOT COMPILED: no Kotlin toolchain on the research machine; written
// from memory.
// Kotlin: Java's exception classes, with the checked/unchecked rule removed. The library
// pairs a throwing form with an `OrNull` form (toInt / toIntOrNull, getValue / get,
// first / firstOrNull); the caller picks one per call.
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket

fun call() = Socket().use { it.connect(InetSocketAddress("127.0.0.1", 1), 300) }

// 1a. A file the user chose.
fun openUserFile(path: String) {
    try {
        println("user-file: ${File(path).readBytes().size} bytes")
    } catch (e: FileNotFoundException) {
        println("user-file: no such file, pick another")
    } catch (e: IOException) {
        println("user-file: cannot read: $e")
    }
}

// 1b. The database the service cannot run without. The class is Java's checked IOException,
// but Kotlin asks for nothing: the caller who cannot recover writes no handler.
fun openRequiredFile(path: String): ByteArray = File(path).readBytes()

// 2a. Optional telemetry.   2b. The primary upstream: no handler; the request boundary has one.
fun sendTelemetry() {
    try {
        call()
    } catch (e: IOException) {
        println("telemetry: dropped one sample")
    }
}

fun handleRequest() = call()

// 3a. A number the user typed: the OrNull form.   3b. A constant: the throwing form.
fun parseUser(s: String) {
    val n = s.toIntOrNull() ?: return println("user-number: not a number, ask again")
    println("user-number: $n")
}

val port = "8080".toInt()

// 4a. A user's key: `map[k]` returns null.   4b. The program's own key: `getValue` throws.
val limits = mapOf("retries" to 3)

fun lookupUser(k: String) = println(limits[k]?.let { "user-key: $it" } ?: "user-key: unknown setting")

fun lookupOwn(k: String): Int = limits.getValue(k)
