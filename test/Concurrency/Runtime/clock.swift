// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library)

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

import _Concurrency
import StdlibUnittest

var tests = TestSuite("Time")

@main struct Main {
  static func main() async {
    tests.test("ContinuousClock sleep") {
      let clock = ContinuousClock()
      let elapsed = await clock.measure {
        try! await clock.sleep(until: .now + .milliseconds(100))
      }
      // give a reasonable range of expected elapsed time
      expectTrue(elapsed > .milliseconds(90))
      expectTrue(elapsed < .milliseconds(200))
    }

    tests.test("SuspendingClock sleep") {
      let clock = SuspendingClock()
      let elapsed = await clock.measure {
        try! await clock.sleep(until: .now + .milliseconds(100))
      }
      // give a reasonable range of expected elapsed time
      expectTrue(elapsed > .milliseconds(90))
      expectTrue(elapsed < .milliseconds(200))
    }

    tests.test("duration addition") {
      let d1 = Duration.milliseconds(500)
      let d2 = Duration.milliseconds(500)
      let d3 = Duration.milliseconds(-500)
      let sum = d1 + d2
      expectEqual(sum, .seconds(1))
      let comps = sum.components
      expectEqual(comps.seconds, 1)
      expectEqual(comps.attoseconds, 0)
      let adjusted = sum + d3
      expectEqual(adjusted, .milliseconds(500))
    }

    tests.test("duration subtraction") {
      let d1 = Duration.nanoseconds(500)
      let d2 = d1 - .nanoseconds(100)
      expectEqual(d2, .nanoseconds(400))
      let d3 = d1 - .nanoseconds(500)
      expectEqual(d3, .nanoseconds(0))
      let d4 = d1 - .nanoseconds(600)
      expectEqual(d4, .nanoseconds(-100))
    }

    tests.test("duration division") {
      let d1 = Duration.seconds(1)
      let halfSecond = d1 / 2
      expectEqual(halfSecond, .milliseconds(500))
    }

    tests.test("duration multiplication") {
      let d1 = Duration.seconds(1)
      let twoSeconds = d1 * 2
      expectEqual(twoSeconds, .seconds(2))
    }

    await runAllTestsAsync()
  }
}