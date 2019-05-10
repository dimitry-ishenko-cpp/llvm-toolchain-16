//===- NestedMatcher.cpp - NestedMatcher Impl  ------------------*- C++ -*-===//
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

#include "mlir/Analysis/NestedMatcher.h"
#include "mlir/AffineOps/AffineOps.h"
#include "mlir/StandardOps/Ops.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

llvm::BumpPtrAllocator *&NestedMatch::allocator() {
  thread_local llvm::BumpPtrAllocator *allocator = nullptr;
  return allocator;
}

NestedMatch NestedMatch::build(Operation *operation,
                               ArrayRef<NestedMatch> nestedMatches) {
  auto *result = allocator()->Allocate<NestedMatch>();
  auto *children = allocator()->Allocate<NestedMatch>(nestedMatches.size());
  std::uninitialized_copy(nestedMatches.begin(), nestedMatches.end(), children);
  new (result) NestedMatch();
  result->matchedOperation = operation;
  result->matchedChildren =
      ArrayRef<NestedMatch>(children, nestedMatches.size());
  return *result;
}

llvm::BumpPtrAllocator *&NestedPattern::allocator() {
  thread_local llvm::BumpPtrAllocator *allocator = nullptr;
  return allocator;
}

NestedPattern::NestedPattern(ArrayRef<NestedPattern> nested,
                             FilterFunctionType filter)
    : nestedPatterns(), filter(filter), skip(nullptr) {
  if (!nested.empty()) {
    auto *newNested = allocator()->Allocate<NestedPattern>(nested.size());
    std::uninitialized_copy(nested.begin(), nested.end(), newNested);
    nestedPatterns = ArrayRef<NestedPattern>(newNested, nested.size());
  }
}

unsigned NestedPattern::getDepth() const {
  if (nestedPatterns.empty()) {
    return 1;
  }
  unsigned depth = 0;
  for (auto &c : nestedPatterns) {
    depth = std::max(depth, c.getDepth());
  }
  return depth + 1;
}

/// Matches a single operation in the following way:
///   1. checks the kind of operation against the matcher, if different then
///      there is no match;
///   2. calls the customizable filter function to refine the single operation
///      match with extra semantic constraints;
///   3. if all is good, recursivey matches the nested patterns;
///   4. if all nested match then the single operation matches too and is
///      appended to the list of matches;
///   5. TODO(ntv) Optionally applies actions (lambda), in which case we will
///      want to traverse in post-order DFS to avoid invalidating iterators.
void NestedPattern::matchOne(Operation *op,
                             SmallVectorImpl<NestedMatch> *matches) {
  if (skip == op) {
    return;
  }
  // Local custom filter function
  if (!filter(*op)) {
    return;
  }

  if (nestedPatterns.empty()) {
    SmallVector<NestedMatch, 8> nestedMatches;
    matches->push_back(NestedMatch::build(op, nestedMatches));
    return;
  }
  // Take a copy of each nested pattern so we can match it.
  for (auto nestedPattern : nestedPatterns) {
    SmallVector<NestedMatch, 8> nestedMatches;
    // Skip elem in the walk immediately following. Without this we would
    // essentially need to reimplement walk here.
    nestedPattern.skip = op;
    nestedPattern.match(op, &nestedMatches);
    // If we could not match even one of the specified nestedPattern, early exit
    // as this whole branch is not a match.
    if (nestedMatches.empty()) {
      return;
    }
    matches->push_back(NestedMatch::build(op, nestedMatches));
  }
}

static bool isAffineForOp(Operation &op) { return op.isa<AffineForOp>(); }

static bool isAffineIfOp(Operation &op) { return op.isa<AffineIfOp>(); }

namespace mlir {
namespace matcher {

NestedPattern Op(FilterFunctionType filter) {
  return NestedPattern({}, filter);
}

NestedPattern If(NestedPattern child) {
  return NestedPattern(child, isAffineIfOp);
}
NestedPattern If(FilterFunctionType filter, NestedPattern child) {
  return NestedPattern(child, [filter](Operation &op) {
    return isAffineIfOp(op) && filter(op);
  });
}
NestedPattern If(ArrayRef<NestedPattern> nested) {
  return NestedPattern(nested, isAffineIfOp);
}
NestedPattern If(FilterFunctionType filter, ArrayRef<NestedPattern> nested) {
  return NestedPattern(nested, [filter](Operation &op) {
    return isAffineIfOp(op) && filter(op);
  });
}

NestedPattern For(NestedPattern child) {
  return NestedPattern(child, isAffineForOp);
}
NestedPattern For(FilterFunctionType filter, NestedPattern child) {
  return NestedPattern(
      child, [=](Operation &op) { return isAffineForOp(op) && filter(op); });
}
NestedPattern For(ArrayRef<NestedPattern> nested) {
  return NestedPattern(nested, isAffineForOp);
}
NestedPattern For(FilterFunctionType filter, ArrayRef<NestedPattern> nested) {
  return NestedPattern(
      nested, [=](Operation &op) { return isAffineForOp(op) && filter(op); });
}

bool isLoadOrStore(Operation &op) {
  return op.isa<LoadOp>() || op.isa<StoreOp>();
}

} // end namespace matcher
} // end namespace mlir
