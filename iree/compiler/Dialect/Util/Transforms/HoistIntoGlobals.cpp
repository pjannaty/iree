// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/Util/Analysis/Constant/ConstExpr.h"
#include "iree/compiler/Dialect/Util/Analysis/Constant/OpOracle.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "iree/compiler/Dialect/Util/Transforms/Passes.h"
#include "llvm/Support/Debug.h"
#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/SymbolTable.h"

#define DEBUG_TYPE "iree-util-hoist-into-globals"

using llvm::dbgs;

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Util {
namespace {

// Maps an original value in the program to the symbol name of a global.
using HoistedValueMap = llvm::DenseMap<Value, GlobalOp>;

// expressions into globals. It is not expected that such a greedy algorithm
// is great, but it is simple. Naive use of this algorithm very likely
// favors programs that consume more memory at runtime than is strictly
// necessary. Either this algorithm can be made smarter or a follow-on pass
// can sink globals into the program where it is profitable to reduce
// working set size.
class HoistIntoGlobalsPass
    : public PassWrapper<HoistIntoGlobalsPass, OperationPass<mlir::ModuleOp>> {
 public:
  StringRef getArgument() const override {
    return "iree-util-hoist-into-globals";
  }

  StringRef getDescription() const override {
    return "Greedily hoists eligible constant expressions into globals";
  }

  void getDependentDialects(DialectRegistry &registry) const override {
    registerConstExprDependentDialects(registry);
  }

  void runOnOperation() override {
    SymbolTable moduleSymbols(getOperation());
    const auto &constExprs = getAnalysis<ConstExprAnalysis>();
    LLVM_DEBUG(dbgs() << constExprs);
    LLVM_DEBUG(dbgs() << "\n\n");

    // Maps original values to newly materialized values.
    HoistedValueMap hoistedMap;

    // Walk all operations in the program and hoist any escapes from
    // const-expr values into globals. Note that we must walk the const-exprs
    // in topological order so that corresponding initializers will be created
    // in order without depending on globals that have not been initialized
    // yet.
    getOperation().walk<WalkOrder::PreOrder>([&](Operation *iterOp) {
      // We only want to look at const-expr ops (non roots) since they may
      // have interesting escapes.
      auto *iterInfo = constExprs.lookup(iterOp);
      if (!iterInfo || iterInfo->isRoot || !iterInfo->isConstExpr()) {
        return WalkResult::advance();
      }

      // Skip if our policy prohibits this being treated as a hoistable leaf.
      if (!isHoistableConstExprLeaf(iterInfo)) return WalkResult::advance();

      // This op is hoistable - for each result find eligible escapes and
      // hoist them.
      LLVM_DEBUG(dbgs() << "PROCESSING CONST-EXPR OP: " << *iterOp << "\n");
      for (Value constExprResult : iterOp->getResults()) {
        // Need to snapshot the uses since we modify them during iteration.
        SmallVector<OpOperand *> uses;
        for (OpOperand &use : constExprResult.getUses()) {
          uses.push_back(&use);
        }

        // Iterate over uses snapshots.
        for (OpOperand *operand : uses) {
          auto *targetInfo = constExprs.lookup(operand->getOwner());

          // We do not treat is as a const escape if the target is:
          //   - Not a valid operand to convert to a constant.
          //   - Const-expr and is an allowed leaf by policy
          // Note that we never touch an operand that is part of a const-expr.
          if (targetInfo && targetInfo->isConstExpr() &&
              isHoistableConstExprLeaf(targetInfo)) {
            LLVM_DEBUG(dbgs() << "  - SKIP (CONST-EXPR): "
                              << *operand->getOwner() << "\n");
            continue;
          }
          if (!isHoistableConstExprConsumingOperand(operand)) {
            LLVM_DEBUG(dbgs() << "  - SKIP (INVALID OPERAND): "
                              << *operand->getOwner() << "\n");
            continue;
          }

          // Bingo.
          LLVM_DEBUG(dbgs() << "  + HOIST CONST-EXPR:\n");
          LLVM_DEBUG(dbgs() << "    : Operand #" << operand->getOperandNumber()
                            << "   of " << *operand->getOwner() << "\n");
          LLVM_DEBUG(dbgs() << "    : From " << operand->get() << "\n\n");

          hoistConstExpr(operand, hoistedMap, moduleSymbols, constExprs);
        }
      }

      return WalkResult::advance();
    });

    cleanupDeadOps(constExprs);
  }

  void hoistConstExpr(OpOperand *operand, HoistedValueMap &hoistedMap,
                      SymbolTable &moduleSymbols,
                      const ConstExprAnalysis &constExprs) {
    Operation *targetOp = operand->getOwner();
    Value originalValue = operand->get();
    GlobalOp existingGlobal = hoistedMap.lookup(originalValue);

    if (!existingGlobal) {
      // No existing mapping.
      Location loc = originalValue.getLoc();
      OpBuilder builder = getModuleEndBuilder();
      auto initializerOp = builder.create<InitializerOp>(loc);
      cloneConstExprInto(initializerOp, originalValue, hoistedMap,
                         moduleSymbols);

      existingGlobal = hoistedMap.lookup(originalValue);
    }
    assert(existingGlobal &&
           "hoisting const-expr should have mapped a global for the requested "
           "value");

    // Already hoisted - just convert to a load.
    OpBuilder builder(targetOp);
    auto load =
        builder.create<GlobalLoadOp>(targetOp->getLoc(), existingGlobal);
    operand->set(load);
  }

  // Clones the const expr tree rooted at `constExprValue` into the given
  // initializer, noting any new hoisted value mappings that result. At
  // a minimum, a mapping will be created for the requested value.
  void cloneConstExprInto(InitializerOp initializerOp, Value constExprValue,
                          HoistedValueMap &hoistedMap,
                          SymbolTable &moduleSymbols) {
    Block *entryBlock = initializerOp.addEntryBlock();
    OpBuilder initBuilder = OpBuilder::atBlockEnd(entryBlock);

    // Clone all dependents of the defining op.
    Operation *rootOp = constExprValue.getDefiningOp();
    assert(rootOp && "const-expr value should have a defining op");
    SetVector<Operation *> slice;
    getBackwardSlice(rootOp, &slice);
    BlockAndValueMapping cloneMap;

    for (Operation *sourceOp : slice) {
      // Iterate over the source results and see if we have already hoisted.
      // Note that because we hoist all results of an op below, we can count
      // on all or none of them having hoisted. Initialization order is
      // correct because we greedily hoist in topological order of const-expr
      // ops above.
      bool needsClone = true;
      for (Value origResult : sourceOp->getResults()) {
        GlobalOp existingGlobal = hoistedMap.lookup(origResult);
        if (!existingGlobal) break;
        needsClone = false;
        cloneMap.map(origResult, initBuilder.create<GlobalLoadOp>(
                                     existingGlobal.getLoc(), existingGlobal));
      }

      if (needsClone) {
        LLVM_DEBUG(dbgs() << "    CLONE OP: " << *sourceOp << "\n");
        Operation *cloneOp = sourceOp->clone(cloneMap);
        initBuilder.insert(cloneOp);
      }
    }

    // Now, for the defining op itself, create a global for each result and
    // store into it.
    // Note that we create globals at the beginning of the module because
    // they must precede accesses and this is guaranteed here.
    OpBuilder globalBuilder = getModuleBeginBuilder();
    Operation *clonedRootOp = rootOp->clone(cloneMap);
    initBuilder.insert(clonedRootOp);
    for (Value origResult : rootOp->getResults()) {
      Value clonedResult = cloneMap.lookup(origResult);
      Location loc = clonedRootOp->getLoc();
      GlobalOp globalOp = globalBuilder.create<GlobalOp>(loc, "hoisted", false,
                                                         origResult.getType());
      StringAttr globalSymbol = moduleSymbols.insert(globalOp);
      SymbolTable::setSymbolVisibility(globalOp,
                                       SymbolTable::Visibility::Private);

      // Save the mapping for the future.
      hoistedMap[origResult] = globalOp;

      // And store into it.
      initBuilder.create<GlobalStoreOp>(loc, clonedResult, globalSymbol);
    }

    initBuilder.create<InitializerReturnOp>(initializerOp.getLoc());
  }

  void cleanupDeadOps(const ConstExprAnalysis &constExprs) {
    llvm::DenseSet<Operation *> allOps;
    constExprs.populateConstExprOperations(allOps);

    // Since we are mutating the const-expr ops, the ConstExprAnalysis will no
    // longer be valid after this point.
    SmallVector<Operation *> worklist;
    worklist.reserve(allOps.size());
    bool madeChanges = true;
    while (madeChanges) {
      madeChanges = false;

      // Prepare worklist.
      worklist.clear();
      worklist.append(allOps.begin(), allOps.end());

      for (Operation *checkOp : worklist) {
        if (checkOp->use_empty()) {
          // Bingo.
          LLVM_DEBUG(dbgs() << "ERASE DEAD OP: " << *checkOp << "\n");
          madeChanges = true;
          allOps.erase(checkOp);
          checkOp->erase();
        }
      }
    }
  }

  OpBuilder getModuleBeginBuilder() {
    Block *block = getOperation().getBody();
    return OpBuilder::atBlockBegin(block);
  }

  OpBuilder getModuleEndBuilder() {
    Block *block = getOperation().getBody();
    return OpBuilder::atBlockEnd(block);
  }
};

}  // namespace

std::unique_ptr<OperationPass<mlir::ModuleOp>> createHoistIntoGlobalsPass() {
  return std::make_unique<HoistIntoGlobalsPass>();
}

static PassRegistration<HoistIntoGlobalsPass> pass;

}  // namespace Util
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir