//===--- SourceLoc.swift - SourceLoc bridiging utilities ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

public struct SourceLoc {
  /// Points into a source file.
  let locationInFile: UnsafePointer<UInt8>

  public init?(locationInFile: UnsafePointer<UInt8>?) {
    guard let locationInFile = locationInFile else {
      return nil
    }
    self.locationInFile = locationInFile
  }

  public init?(bridged: BridgedSourceLoc) {
    guard let locationInFile = bridged.pointer else {
      return nil
    }
    self.init(locationInFile: locationInFile)
  }

  public var bridged: BridgedSourceLoc {
    .init(pointer: locationInFile)
  }
}

extension SourceLoc {
  public func advanced(by n: Int) -> SourceLoc {
    SourceLoc(locationInFile: locationInFile.advanced(by: n))!
  }
}

extension Optional where Wrapped == SourceLoc {
  public var bridged: BridgedSourceLoc {
    self?.bridged ?? .init(pointer: nil)
  }
}

public struct CharSourceRange {
  private let start: SourceLoc
  private let byteLength: Int

  public init(start: SourceLoc, byteLength: Int) {
    self.start = start
    self.byteLength = byteLength
  }

  public init?(bridged: BridgedCharSourceRange) {
    guard let start = SourceLoc(bridged: bridged.start) else {
      return nil
    }
    self.init(start: start, byteLength: bridged.byteLength)
  }

  public var bridged: BridgedCharSourceRange {
    .init(start: start.bridged, byteLength: byteLength)
  }
}

extension Optional where Wrapped == CharSourceRange {
  public var bridged: BridgedCharSourceRange {
    self?.bridged ?? .init(start: .init(pointer: nil), byteLength: 0)
  }
}
