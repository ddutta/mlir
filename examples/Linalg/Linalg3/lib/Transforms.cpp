//===- Transforms.cpp - Implementation of the linalg Transformations ------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements analyses and transformations for the linalg dialect.
//
//===----------------------------------------------------------------------===//

#include "linalg3/Transforms.h"
#include "linalg2/Intrinsics.h"
#include "linalg3/Ops.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/StandardTypes.h"

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace linalg;
using namespace linalg::intrinsics;

void linalg::composeSliceOps(mlir::Function *f) {
  f->walkPostOrder<SliceOp>([](SliceOp sliceOp) {
    auto *sliceResult = sliceOp.getResult();
    auto viewOp = createFullyComposedView(sliceResult);
    sliceResult->replaceAllUsesWith(viewOp.getResult());
    sliceOp.erase();
  });
}

void linalg::lowerToFinerGrainedTensorContraction(mlir::Function *f) {
  f->walkPostOrder([](Operation *op) {
    if (auto matmulOp = op->dyn_cast<linalg::MatmulOp>()) {
      matmulOp.writeAsFinerGrainTensorContraction();
    } else if (auto matvecOp = op->dyn_cast<linalg::MatvecOp>()) {
      matvecOp.writeAsFinerGrainTensorContraction();
    } else {
      return;
    }
    op->erase();
  });
}

// Folding eagerly is necessary to abide by affine.for static step requirement.
// Returns nullptr if folding is not trivially feasible.
static Value *tryFold(AffineMap map, SmallVector<Value *, 4> operands) {
  assert(map.getNumResults() == 1 && "single result map expected");
  auto expr = map.getResult(0);
  if (auto dim = expr.dyn_cast<AffineDimExpr>())
    return operands[dim.getPosition()];
  if (auto sym = expr.dyn_cast<AffineSymbolExpr>())
    return operands[map.getNumDims() + sym.getPosition()];
  if (auto cst = expr.dyn_cast<AffineConstantExpr>())
    return constant_index(cst.getValue());
  return nullptr;
}

static Value *makeFoldedComposedAffineApply(AffineMap map,
                                            ArrayRef<Value *> operandsRef) {
  SmallVector<Value *, 4> operands(operandsRef.begin(), operandsRef.end());
  fullyComposeAffineMapAndOperands(&map, &operands);
  if (auto *v = tryFold(map, operands)) {
    return v;
  }
  auto *b = ScopedContext::getBuilder();
  auto loc = ScopedContext::getLocation();
  return b->create<AffineApplyOp>(loc, map, operands).getResult();
}

struct RangeParts {
  explicit RangeParts(unsigned reserved);
  RangeParts(ArrayRef<Value *> ranges);

  SmallVector<Value *, 4> makeRanges();

  SmallVector<Value *, 4> mins;
  SmallVector<Value *, 4> maxes;
  SmallVector<Value *, 4> steps;
};

RangeParts::RangeParts(unsigned reserved) {
  mins.reserve(reserved);
  maxes.reserve(reserved);
  steps.reserve(reserved);
}

static SmallVector<Value *, 4>
extractFromRanges(ArrayRef<Value *> ranges,
                  std::function<Value *(RangeOp)> extract) {
  SmallVector<Value *, 4> res;
  res.reserve(ranges.size());
  for (auto *v : ranges) {
    auto r = v->getDefiningOp()->cast<RangeOp>();
    res.push_back(extract(r));
  }
  return res;
}

RangeParts::RangeParts(ArrayRef<Value *> ranges)
    : mins(extractFromRanges(ranges, [](RangeOp r) { return r.getMin(); })),
      maxes(extractFromRanges(ranges, [](RangeOp r) { return r.getMax(); })),
      steps(extractFromRanges(ranges, [](RangeOp r) { return r.getStep(); })) {}

SmallVector<Value *, 4> RangeParts::makeRanges() {
  SmallVector<Value *, 4> res;
  res.reserve(mins.size());
  for (auto z : llvm::zip(mins, maxes, steps)) {
    res.push_back(range(std::get<0>(z), std::get<1>(z), std::get<2>(z)));
  }
  return res;
}

static RangeParts makeGenericRangeParts(AffineMap map,
                                        ArrayRef<Value *> ranges) {
  assert(map.getNumInputs() == ranges.size());
  unsigned numDims = map.getNumDims();
  assert(map.getNumSymbols() == 0);
  assert(map.getRangeSizes().empty());

  RangeParts res(map.getNumResults());
  RangeParts rangeParts(ranges);
  for (auto expr : map.getResults()) {
    AffineMap map = AffineMap::get(numDims, 0, expr, {});
    res.mins.push_back(makeFoldedComposedAffineApply(map, rangeParts.mins));
    res.maxes.push_back(makeFoldedComposedAffineApply(map, rangeParts.maxes));
    res.steps.push_back(makeFoldedComposedAffineApply(map, rangeParts.steps));
  }
  return res;
}

SmallVector<Value *, 4> makeGenericRanges(AffineMap map,
                                          ArrayRef<Value *> ranges) {
  return makeGenericRangeParts(map, ranges).makeRanges();
}

static SmallVector<Value *, 4> makeGenericLoopRanges(
    AffineMap operandRangesToLoopsMap, ArrayRef<Value *> ranges,
    llvm::Optional<ArrayRef<Value *>> tileSizes = llvm::None) {
  RangeParts res = makeGenericRangeParts(operandRangesToLoopsMap, ranges);
  if (!tileSizes.hasValue())
    return res.makeRanges();
  SmallVector<Value *, 4> tiledSteps;
  for (auto z : llvm::zip(res.steps, *tileSizes)) {
    auto *step = std::get<0>(z);
    auto tileSize = std::get<1>(z);
    auto stepValue = step->getDefiningOp()->cast<ConstantIndexOp>().getValue();
    auto tileSizeValue =
        tileSize->getDefiningOp()->cast<ConstantIndexOp>().getValue();
    assert(stepValue > 0);
    tiledSteps.push_back(constant_index(stepValue * tileSizeValue));
  }
  res.steps = tiledSteps;
  return res.makeRanges();
}

template <class ContractionOp>
static SmallVector<mlir::AffineForOp, 4>
writeAsLoops(ContractionOp contraction) {
  ScopedContext scope(mlir::FuncBuilder(contraction.getOperation()),
                      contraction.getLoc());
  auto loopRanges = makeGenericLoopRanges(operandRangesToLoopsMap(contraction),
                                          getRanges(contraction));

  SmallVector<IndexHandle, 4> parallelIvs(contraction.getNumParallelDims());
  SmallVector<IndexHandle, 4> reductionIvs(contraction.getNumReductionDims());
  auto pivs = IndexHandle::makeIndexHandlePointers(parallelIvs);
  auto rivs = IndexHandle::makeIndexHandlePointers(reductionIvs);
  assert(loopRanges.size() == pivs.size() + rivs.size());

  // clang-format off
  using linalg::common::LoopNestRangeBuilder;
  ArrayRef<Value *> ranges(loopRanges);
  LoopNestRangeBuilder(pivs, ranges.take_front(pivs.size()))({
    LoopNestRangeBuilder(rivs, ranges.take_back(rivs.size()))({
      [&contraction, &parallelIvs, &reductionIvs]() {
        SmallVector<mlir::Value *, 4> parallel(
            parallelIvs.begin(), parallelIvs.end());
        SmallVector<mlir::Value *, 4> reduction(
            reductionIvs.begin(), reductionIvs.end());
        contraction.emitScalarImplementation(parallel, reduction);
        /// NestedBuilders expect handles, we thus return an IndexHandle.
        return IndexHandle();
      }()
    })
  });
  // clang-format on

  SmallVector<mlir::AffineForOp, 4> res;
  res.reserve(pivs.size() + rivs.size());
  for (auto iv : parallelIvs)
    res.push_back(getForInductionVarOwner(iv.getValue()));
  for (auto iv : reductionIvs)
    res.push_back(getForInductionVarOwner(iv.getValue()));
  return res;
}

void linalg::lowerToLoops(mlir::Function *f) {
  f->walkPostOrder([](Operation *op) {
    if (auto matmulOp = op->dyn_cast<linalg::MatmulOp>()) {
      writeAsLoops(matmulOp);
    } else if (auto matvecOp = op->dyn_cast<linalg::MatvecOp>()) {
      writeAsLoops(matvecOp);
    } else if (auto dotOp = op->dyn_cast<linalg::DotOp>()) {
      writeAsLoops(dotOp);
    } else {
      return;
    }
    op->erase();
  });
}
