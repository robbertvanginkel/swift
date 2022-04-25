// swift-tools-version:5.3

import PackageDescription

let package = Package(
  name: "SwiftCompilerSources",
  platforms: [
    .macOS("10.9"),
  ],
  products: [
    .library(
      name: "Swift",
      type: .static,
      targets: ["SIL", "Optimizer", "_CompilerRegexParser"]),
  ],
  dependencies: [
  ],
  // Note that all modules must be added to LIBSWIFT_MODULES in the top-level
  // CMakeLists.txt file to get debugging working.
  targets: [
    .target(
      name: "SIL",
      dependencies: [],
      swiftSettings: [SwiftSetting.unsafeFlags([
          "-I", "../include/swift",
          "-cross-module-optimization"
        ])]),
    .target(
      name: "_CompilerRegexParser",
      dependencies: [],
      path: "_RegexParser",
      swiftSettings: [
        .unsafeFlags([
          "-I", "../include/swift",
          "-cross-module-optimization",
        ]),
        // Workaround until `_RegexParser` is imported as implementation-only
        // by `_StringProcessing`.
        .unsafeFlags([
          "-Xfrontend",
          "-disable-implicit-string-processing-module-import"
        ])]),
    .target(
      name: "Optimizer",
      dependencies: ["SIL", "_CompilerRegexParser"],
      swiftSettings: [SwiftSetting.unsafeFlags([
          "-I", "../include/swift",
          "-cross-module-optimization"
        ])]),
  ]
)
