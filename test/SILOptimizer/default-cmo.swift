
// RUN: %empty-directory(%t) 

// RUN: %target-build-swift -O -wmo -parse-as-library -emit-module -emit-module-path=%t/Submodule.swiftmodule -module-name=Submodule %S/Inputs/cross-module/default-submodule.swift -c -o %t/submodule.o
// RUN: %target-build-swift -O -wmo -parse-as-library -emit-module -emit-module-path=%t/Module.swiftmodule -module-name=Module -I%t -I%S/Inputs/cross-module %S/Inputs/cross-module/default-module.swift -c -o %t/module.o
// RUN: %target-build-swift -O -wmo -parse-as-library -emit-tbd -emit-tbd-path %t/ModuleTBD.tbd -emit-module -emit-module-path=%t/ModuleTBD.swiftmodule -module-name=ModuleTBD -I%t -I%S/Inputs/cross-module %S/Inputs/cross-module/default-module.swift -c -o %t/moduletbd.o

// RUN: %target-build-swift -O -wmo -module-name=Main -I%t %s -emit-sil | %FileCheck %s

// REQUIRES: cmo_enabled

import Module
import ModuleTBD

// CHECK-LABEL: sil @$s4Main11doIncrementyS2iF
// CHECK-NOT:     function_ref 
// CHECK-NOT:     apply 
// CHECK:       } // end sil function '$s4Main11doIncrementyS2iF'
public func doIncrement(_ x: Int) -> Int {
  return Module.incrementByThree(x)
}

// CHECK-LABEL: sil @$s4Main19doIncrementWithCallyS2iF
// CHECK:         function_ref @$s9Submodule19incrementByOneNoCMOyS2iF
// CHECK:       } // end sil function '$s4Main19doIncrementWithCallyS2iF'
public func doIncrementWithCall(_ x: Int) -> Int {
  return Module.incrementByThreeWithCall(x)
}

// CHECK-LABEL: sil @$s4Main14doIncrementTBDyS2iF
// CHECK-NOT:     function_ref 
// CHECK-NOT:     apply 
// CHECK:       } // end sil function '$s4Main14doIncrementTBDyS2iF'
public func doIncrementTBD(_ x: Int) -> Int {
  return ModuleTBD.incrementByThree(x)
}

// CHECK-LABEL: sil @$s4Main22doIncrementTBDWithCallyS2iF
// CHECK:         function_ref @$s9ModuleTBD24incrementByThreeWithCallyS2iF
// CHECK:       } // end sil function '$s4Main22doIncrementTBDWithCallyS2iF'
public func doIncrementTBDWithCall(_ x: Int) -> Int {
  return ModuleTBD.incrementByThreeWithCall(x)
}

