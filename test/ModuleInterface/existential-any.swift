// RUN: %target-swift-frontend-typecheck -swift-version 5 -enable-library-evolution -emit-module-interface-path %t.swiftinterface %s -module-name main
// RUN: %target-swift-frontend -typecheck-module-from-interface %t.swiftinterface -module-name main
// RUN: %FileCheck %s < %t.swiftinterface

// CHECK: public protocol P
public protocol P { }

// CHECK: public protocol Q
public protocol Q {
  // CHECK: associatedtype A : main.P
  associatedtype A: P
}

// CHECK: public func takesAndReturnsP(_ x: any main.P) -> any main.P
public func takesAndReturnsP(_ x: P) -> P {
  return x
}

// CHECK: public func takesAndReturnsOptionalP(_ x: (any main.P)?) -> (any main.P)?
public func takesAndReturnsOptionalP(_ x: P?) -> P? {
  return x
}

// CHECK: public func takesAndReturnsQ(_ x: any main.Q) -> any main.Q
public func takesAndReturnsQ(_ x: any Q) -> any Q {
  return x
}

// CHECK: public struct S
public struct S {
  // CHECK: public var p: any main.P
  public var p: P
  // CHECK: public var maybeP: (any main.P)?
  public var maybeP: P?
  // CHECK: public var q: any main.Q
  public var q: any Q
}
