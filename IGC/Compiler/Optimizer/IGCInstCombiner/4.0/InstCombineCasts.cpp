/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/
//===- InstCombineCasts.cpp -----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the visit functions for cast operations.
//
//===----------------------------------------------------------------------===//
#include "common/LLVMWarningsPush.hpp"
#include "InstCombineInternal.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Analysis/TargetLibraryInfo.h"


using namespace llvm;
using namespace PatternMatch;
using namespace IGCombiner;


#define DEBUG_TYPE "instcombine"

/// Analyze 'Val', seeing if it is a simple linear expression.
/// If so, decompose it, returning some value X, such that Val is
/// X*Scale+Offset.
///
static Value *decomposeSimpleLinearExpr(Value *Val, unsigned &Scale,
                                        uint64_t &Offset) {
  if (ConstantInt *CI = dyn_cast<ConstantInt>(Val)) {
    Offset = CI->getZExtValue();
    Scale  = 0;
    return ConstantInt::get(Val->getType(), 0);
  }

  if (BinaryOperator *I = dyn_cast<BinaryOperator>(Val)) {
    // Cannot look past anything that might overflow.
    OverflowingBinaryOperator *OBI = dyn_cast<OverflowingBinaryOperator>(Val);
    if (OBI && !OBI->hasNoUnsignedWrap() && !OBI->hasNoSignedWrap()) {
      Scale = 1;
      Offset = 0;
      return Val;
    }

    if (ConstantInt *RHS = dyn_cast<ConstantInt>(I->getOperand(1))) {
      if (I->getOpcode() == Instruction::Shl) {
        // This is a value scaled by '1 << the shift amt'.
        Scale = UINT64_C(1) << RHS->getZExtValue();
        Offset = 0;
        return I->getOperand(0);
      }

      if (I->getOpcode() == Instruction::Mul) {
        // This value is scaled by 'RHS'.
        Scale = RHS->getZExtValue();
        Offset = 0;
        return I->getOperand(0);
      }

      if (I->getOpcode() == Instruction::Add) {
        // We have X+C.  Check to see if we really have (X*C2)+C1,
        // where C1 is divisible by C2.
        unsigned SubScale = 0;
        Value *SubVal =
          decomposeSimpleLinearExpr(I->getOperand(0), SubScale, Offset);
        Offset += RHS->getZExtValue();
        Scale = SubScale;
        return SubVal;
      }
    }
  }

  // Otherwise, we can't look past this.
  Scale = 1;
  Offset = 0;
  return Val;
}

/// If we find a cast of an allocation instruction, try to eliminate the cast by
/// moving the type information into the alloc.
Instruction *InstCombiner::PromoteCastOfAllocation(BitCastInst &CI,
                                                   AllocaInst &AI) {
  PointerType *PTy = cast<PointerType>(CI.getType());

  BuilderTy AllocaBuilder(*Builder);
  AllocaBuilder.SetInsertPoint(&AI);

  // Get the type really allocated and the type casted to.
  Type *AllocElTy = AI.getAllocatedType();
  Type *CastElTy = PTy->getElementType();
  if (!AllocElTy->isSized() || !CastElTy->isSized()) return nullptr;

  unsigned AllocElTyAlign = DL.getABITypeAlignment(AllocElTy);
  unsigned CastElTyAlign = DL.getABITypeAlignment(CastElTy);
  if (CastElTyAlign < AllocElTyAlign) return nullptr;

  // If the allocation has multiple uses, only promote it if we are strictly
  // increasing the alignment of the resultant allocation.  If we keep it the
  // same, we open the door to infinite loops of various kinds.
  if (!AI.hasOneUse() && CastElTyAlign == AllocElTyAlign) return nullptr;

  uint64_t AllocElTySize = DL.getTypeAllocSize(AllocElTy);
  uint64_t CastElTySize = DL.getTypeAllocSize(CastElTy);
  if (CastElTySize == 0 || AllocElTySize == 0) return nullptr;

  // If the allocation has multiple uses, only promote it if we're not
  // shrinking the amount of memory being allocated.
  uint64_t AllocElTyStoreSize = DL.getTypeStoreSize(AllocElTy);
  uint64_t CastElTyStoreSize = DL.getTypeStoreSize(CastElTy);
  if (!AI.hasOneUse() && CastElTyStoreSize < AllocElTyStoreSize) return nullptr;

  // See if we can satisfy the modulus by pulling a scale out of the array
  // size argument.
  unsigned ArraySizeScale;
  uint64_t ArrayOffset;
  Value *NumElements = // See if the array size is a decomposable linear expr.
    decomposeSimpleLinearExpr(AI.getOperand(0), ArraySizeScale, ArrayOffset);

  // If we can now satisfy the modulus, by using a non-1 scale, we really can
  // do the xform.
  if ((AllocElTySize*ArraySizeScale) % CastElTySize != 0 ||
      (AllocElTySize*ArrayOffset   ) % CastElTySize != 0) return nullptr;

  unsigned Scale = (AllocElTySize*ArraySizeScale)/CastElTySize;
  Value *Amt = nullptr;
  if (Scale == 1) {
    Amt = NumElements;
  } else {
    Amt = ConstantInt::get(AI.getArraySize()->getType(), Scale);
    // Insert before the alloca, not before the cast.
    Amt = AllocaBuilder.CreateMul(Amt, NumElements);
  }

  if (uint64_t Offset = (AllocElTySize*ArrayOffset)/CastElTySize) {
    Value *Off = ConstantInt::get(AI.getArraySize()->getType(),
                                  Offset, true);
    Amt = AllocaBuilder.CreateAdd(Amt, Off);
  }

  AllocaInst *New = AllocaBuilder.CreateAlloca(CastElTy, Amt);
  New->setAlignment(AI.getAlignment());
  New->takeName(&AI);
  New->setUsedWithInAlloca(AI.isUsedWithInAlloca());

  // If the allocation has multiple real uses, insert a cast and change all
  // things that used it to use the new cast.  This will also hack on CI, but it
  // will die soon.
  if (!AI.hasOneUse()) {
    // New is the allocation instruction, pointer typed. AI is the original
    // allocation instruction, also pointer typed. Thus, cast to use is BitCast.
    Value *NewCast = AllocaBuilder.CreateBitCast(New, AI.getType(), "tmpcast");
    replaceInstUsesWith(AI, NewCast);
  }
  return replaceInstUsesWith(CI, New);
}

/// Given an expression that CanEvaluateTruncated or CanEvaluateSExtd returns
/// true for, actually insert the code to evaluate the expression.
Value *InstCombiner::EvaluateInDifferentType(Value *V, Type *Ty,
                                             bool isSigned) {
  if (Constant *C = dyn_cast<Constant>(V)) {
    C = ConstantExpr::getIntegerCast(C, Ty, isSigned /*Sext or ZExt*/);
    // If we got a constantexpr back, try to simplify it with DL info.
    if (Constant *FoldedC = ConstantFoldConstant(C, DL, &TLI))
      C = FoldedC;
    return C;
  }

  // Otherwise, it must be an instruction.
  Instruction *I = cast<Instruction>(V);
  Instruction *Res = nullptr;
  unsigned Opc = I->getOpcode();
  switch (Opc) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::AShr:
  case Instruction::LShr:
  case Instruction::Shl:
  case Instruction::UDiv:
  case Instruction::URem: {
    Value *LHS = EvaluateInDifferentType(I->getOperand(0), Ty, isSigned);
    Value *RHS = EvaluateInDifferentType(I->getOperand(1), Ty, isSigned);
    Res = BinaryOperator::Create((Instruction::BinaryOps)Opc, LHS, RHS);
    break;
  }
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
    // If the source type of the cast is the type we're trying for then we can
    // just return the source.  There's no need to insert it because it is not
    // new.
    if (I->getOperand(0)->getType() == Ty)
      return I->getOperand(0);

    // Otherwise, must be the same type of cast, so just reinsert a new one.
    // This also handles the case of zext(trunc(x)) -> zext(x).
    Res = CastInst::CreateIntegerCast(I->getOperand(0), Ty,
                                      Opc == Instruction::SExt);
    break;
  case Instruction::Select: {
    Value *True = EvaluateInDifferentType(I->getOperand(1), Ty, isSigned);
    Value *False = EvaluateInDifferentType(I->getOperand(2), Ty, isSigned);
    Res = SelectInst::Create(I->getOperand(0), True, False);
    break;
  }
  case Instruction::PHI: {
    PHINode *OPN = cast<PHINode>(I);
    PHINode *NPN = PHINode::Create(Ty, OPN->getNumIncomingValues());
    for (unsigned i = 0, e = OPN->getNumIncomingValues(); i != e; ++i) {
      Value *V =
          EvaluateInDifferentType(OPN->getIncomingValue(i), Ty, isSigned);
      NPN->addIncoming(V, OPN->getIncomingBlock(i));
    }
    Res = NPN;
    break;
  }
  default:
    // TODO: Can handle more cases here.
    llvm_unreachable("Unreachable!");
  }

  Res->takeName(I);
  return InsertNewInstWith(Res, *I);
}

Instruction::CastOps InstCombiner::isEliminableCastPair(const CastInst *CI1,
                                                        const CastInst *CI2) {
  Type *SrcTy = CI1->getSrcTy();
  Type *MidTy = CI1->getDestTy();
  Type *DstTy = CI2->getDestTy();

  Instruction::CastOps firstOp = Instruction::CastOps(CI1->getOpcode());
  Instruction::CastOps secondOp = Instruction::CastOps(CI2->getOpcode());
  Type *SrcIntPtrTy =
      SrcTy->isPtrOrPtrVectorTy() ? DL.getIntPtrType(SrcTy) : nullptr;
  Type *MidIntPtrTy =
      MidTy->isPtrOrPtrVectorTy() ? DL.getIntPtrType(MidTy) : nullptr;
  Type *DstIntPtrTy =
      DstTy->isPtrOrPtrVectorTy() ? DL.getIntPtrType(DstTy) : nullptr;
  unsigned Res = CastInst::isEliminableCastPair(firstOp, secondOp, SrcTy, MidTy,
                                                DstTy, SrcIntPtrTy, MidIntPtrTy,
                                                DstIntPtrTy);

  // We don't want to form an inttoptr or ptrtoint that converts to an integer
  // type that differs from the pointer size.
  if ((Res == Instruction::IntToPtr && SrcTy != DstIntPtrTy) ||
      (Res == Instruction::PtrToInt && DstTy != SrcIntPtrTy))
    Res = 0;

  return Instruction::CastOps(Res);
}

/// @brief Implement the transforms common to all CastInst visitors.
Instruction *InstCombiner::commonCastTransforms(CastInst &CI) {
  Value *Src = CI.getOperand(0);

  // Try to eliminate a cast of a cast.
  if (auto *CSrc = dyn_cast<CastInst>(Src)) {   // A->B->C cast
    if (Instruction::CastOps NewOpc = isEliminableCastPair(CSrc, &CI)) {
      // The first cast (CSrc) is eliminable so we need to fix up or replace
      // the second cast (CI). CSrc will then have a good chance of being dead.
      return CastInst::Create(NewOpc, CSrc->getOperand(0), CI.getType());
    }
  }

  // If we are casting a select, then fold the cast into the select.
  if (auto *SI = dyn_cast<SelectInst>(Src))
    if (Instruction *NV = FoldOpIntoSelect(CI, SI))
      return NV;

  // If we are casting a PHI, then fold the cast into the PHI.
  if (isa<PHINode>(Src)) {
    // Don't do this if it would create a PHI node with an illegal type from a
    // legal type.
    if (!Src->getType()->isIntegerTy() || !CI.getType()->isIntegerTy() ||
        ShouldChangeType(CI.getType(), Src->getType()))
      if (Instruction *NV = FoldOpIntoPhi(CI))
        return NV;
  }

  return nullptr;
}

/// Return true if we can evaluate the specified expression tree as type Ty
/// instead of its larger type, and arrive with the same value.
/// This is used by code that tries to eliminate truncates.
///
/// Ty will always be a type smaller than V.  We should return true if trunc(V)
/// can be computed by computing V in the smaller type.  If V is an instruction,
/// then trunc(inst(x,y)) can be computed as inst(trunc(x),trunc(y)), which only
/// makes sense if x and y can be efficiently truncated.
///
/// This function works on both vectors and scalars.
///
static bool canEvaluateTruncated(Value *V, Type *Ty, InstCombiner &IC,
                                 Instruction *CxtI) {
  // We can always evaluate constants in another type.
  if (isa<Constant>(V))
    return true;

  Instruction *I = dyn_cast<Instruction>(V);
  if (!I) return false;

  Type *OrigTy = V->getType();

  // If this is an extension from the dest type, we can eliminate it, even if it
  // has multiple uses.
  if ((isa<ZExtInst>(I) || isa<SExtInst>(I)) &&
      I->getOperand(0)->getType() == Ty)
    return true;

  // We can't extend or shrink something that has multiple uses: doing so would
  // require duplicating the instruction in general, which isn't profitable.
  if (!I->hasOneUse()) return false;

  unsigned Opc = I->getOpcode();
  switch (Opc) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    // These operators can all arbitrarily be extended or truncated.
    return canEvaluateTruncated(I->getOperand(0), Ty, IC, CxtI) &&
           canEvaluateTruncated(I->getOperand(1), Ty, IC, CxtI);

  case Instruction::UDiv:
  case Instruction::URem: {
    // UDiv and URem can be truncated if all the truncated bits are zero.
    uint32_t OrigBitWidth = OrigTy->getScalarSizeInBits();
    uint32_t BitWidth = Ty->getScalarSizeInBits();
    if (BitWidth < OrigBitWidth) {
      APInt Mask = APInt::getHighBitsSet(OrigBitWidth, OrigBitWidth-BitWidth);
      if (IC.MaskedValueIsZero(I->getOperand(0), Mask, 0, CxtI) &&
          IC.MaskedValueIsZero(I->getOperand(1), Mask, 0, CxtI)) {
        return canEvaluateTruncated(I->getOperand(0), Ty, IC, CxtI) &&
               canEvaluateTruncated(I->getOperand(1), Ty, IC, CxtI);
      }
    }
    break;
  }
  case Instruction::Shl:
    // If we are truncating the result of this SHL, and if it's a shift of a
    // constant amount, we can always perform a SHL in a smaller type.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(I->getOperand(1))) {
      uint32_t BitWidth = Ty->getScalarSizeInBits();
      if (CI->getLimitedValue(BitWidth) < BitWidth)
        return canEvaluateTruncated(I->getOperand(0), Ty, IC, CxtI);
    }
    break;
  case Instruction::LShr:
    // If this is a truncate of a logical shr, we can truncate it to a smaller
    // lshr iff we know that the bits we would otherwise be shifting in are
    // already zeros.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(I->getOperand(1))) {
      uint32_t OrigBitWidth = OrigTy->getScalarSizeInBits();
      uint32_t BitWidth = Ty->getScalarSizeInBits();
      if (IC.MaskedValueIsZero(I->getOperand(0),
            APInt::getHighBitsSet(OrigBitWidth, OrigBitWidth-BitWidth), 0, CxtI) &&
          CI->getLimitedValue(BitWidth) < BitWidth) {
        return canEvaluateTruncated(I->getOperand(0), Ty, IC, CxtI);
      }
    }
    break;
  case Instruction::Trunc:
    // trunc(trunc(x)) -> trunc(x)
    return true;
  case Instruction::ZExt:
  case Instruction::SExt:
    // trunc(ext(x)) -> ext(x) if the source type is smaller than the new dest
    // trunc(ext(x)) -> trunc(x) if the source type is larger than the new dest
    return true;
  case Instruction::Select: {
    SelectInst *SI = cast<SelectInst>(I);
    return canEvaluateTruncated(SI->getTrueValue(), Ty, IC, CxtI) &&
           canEvaluateTruncated(SI->getFalseValue(), Ty, IC, CxtI);
  }
  case Instruction::PHI: {
    // We can change a phi if we can change all operands.  Note that we never
    // get into trouble with cyclic PHIs here because we only consider
    // instructions with a single use.
    PHINode *PN = cast<PHINode>(I);
    for (Value *IncValue : PN->incoming_values())
      if (!canEvaluateTruncated(IncValue, Ty, IC, CxtI))
        return false;
    return true;
  }
  default:
    // TODO: Can handle more cases here.
    break;
  }

  return false;
}

/// Given a vector that is bitcast to an integer, optionally logically
/// right-shifted, and truncated, convert it to an extractelement.
/// Example (big endian):
///   trunc (lshr (bitcast <4 x i32> %X to i128), 32) to i32
///   --->
///   extractelement <4 x i32> %X, 1
static Instruction *foldVecTruncToExtElt(TruncInst &Trunc, InstCombiner &IC,
                                         const DataLayout &DL) {
  Value *TruncOp = Trunc.getOperand(0);
  Type *DestType = Trunc.getType();
  if (!TruncOp->hasOneUse() || !isa<IntegerType>(DestType))
    return nullptr;

  Value *VecInput = nullptr;
  ConstantInt *ShiftVal = nullptr;
  if (!match(TruncOp, m_CombineOr(m_BitCast(m_Value(VecInput)),
                                  m_LShr(m_BitCast(m_Value(VecInput)),
                                         m_ConstantInt(ShiftVal)))) ||
      !isa<VectorType>(VecInput->getType()))
    return nullptr;

  VectorType *VecType = cast<VectorType>(VecInput->getType());
  unsigned VecWidth = VecType->getPrimitiveSizeInBits();
  unsigned DestWidth = DestType->getPrimitiveSizeInBits();
  unsigned ShiftAmount = ShiftVal ? ShiftVal->getZExtValue() : 0;

  if ((VecWidth % DestWidth != 0) || (ShiftAmount % DestWidth != 0))
    return nullptr;

  // If the element type of the vector doesn't match the result type,
  // bitcast it to a vector type that we can extract from.
  unsigned NumVecElts = VecWidth / DestWidth;
  if (VecType->getElementType() != DestType) {
    VecType = VectorType::get(DestType, NumVecElts);
    VecInput = IC.Builder->CreateBitCast(VecInput, VecType, "bc");
  }

  unsigned Elt = ShiftAmount / DestWidth;
  if (DL.isBigEndian())
    Elt = NumVecElts - 1 - Elt;

  return ExtractElementInst::Create(VecInput, IC.Builder->getInt32(Elt));
}

/// Try to narrow the width of bitwise logic instructions with constants.
Instruction *InstCombiner::shrinkBitwiseLogic(TruncInst &Trunc) {
  Type *SrcTy = Trunc.getSrcTy();
  Type *DestTy = Trunc.getType();
  if (isa<IntegerType>(SrcTy) && !ShouldChangeType(SrcTy, DestTy))
    return nullptr;

  BinaryOperator *LogicOp = nullptr;
  Constant *C = nullptr;
  if (!match(Trunc.getOperand(0), m_OneUse(m_BinOp(LogicOp))) ||
      !LogicOp->isBitwiseLogicOp() ||
      !match(LogicOp->getOperand(1), m_Constant(C)))
    return nullptr;

  // trunc (logic X, C) --> logic (trunc X, C')
  Constant *NarrowC = ConstantExpr::getTrunc(C, DestTy);
  Value *NarrowOp0 = Builder->CreateTrunc(LogicOp->getOperand(0), DestTy);
  return BinaryOperator::Create(LogicOp->getOpcode(), NarrowOp0, NarrowC);
}

Instruction *InstCombiner::visitTrunc(TruncInst &CI) {
  if (Instruction *Result = commonCastTransforms(CI))
    return Result;

  // Test if the trunc is the user of a select which is part of a
  // minimum or maximum operation. If so, don't do any more simplification.
  // Even simplifying demanded bits can break the canonical form of a
  // min/max.
  Value *LHS = nullptr, *RHS = nullptr;
  if (SelectInst *SI = dyn_cast<SelectInst>(CI.getOperand(0)))
    if (matchSelectPattern(SI, LHS, RHS).Flavor != SPF_UNKNOWN)
      return nullptr;

  // See if we can simplify any instructions used by the input whose sole
  // purpose is to compute bits we don't care about.
  if (SimplifyDemandedInstructionBits(CI))
    return &CI;

  Value *Src = CI.getOperand(0);
  Type *DestTy = CI.getType(), *SrcTy = Src->getType();

  // Attempt to truncate the entire input expression tree to the destination
  // type.   Only do this if the dest type is a simple type, don't convert the
  // expression tree to something weird like i93 unless the source is also
  // strange.
  if ((DestTy->isVectorTy() || ShouldChangeType(SrcTy, DestTy)) &&
      canEvaluateTruncated(Src, DestTy, *this, &CI)) {

    // If this cast is a truncate, evaluting in a different type always
    // eliminates the cast, so it is always a win.
    DEBUG(dbgs() << "ICE: EvaluateInDifferentType converting expression type"
          " to avoid cast: " << CI << '\n');
    Value *Res = EvaluateInDifferentType(Src, DestTy, false);
    assert(Res->getType() == DestTy);
    return replaceInstUsesWith(CI, Res);
  }

  // Canonicalize trunc x to i1 -> (icmp ne (and x, 1), 0), likewise for vector.
  if (DestTy->getScalarSizeInBits() == 1) {
    Constant *One = ConstantInt::get(SrcTy, 1);
    Src = Builder->CreateAnd(Src, One);
    Value *Zero = Constant::getNullValue(Src->getType());
    return new ICmpInst(ICmpInst::ICMP_NE, Src, Zero);
  }

  // Transform trunc(lshr (zext A), Cst) to eliminate one type conversion.
  Value *A = nullptr; ConstantInt *Cst = nullptr;
  if (Src->hasOneUse() &&
      match(Src, m_LShr(m_ZExt(m_Value(A)), m_ConstantInt(Cst)))) {
    // We have three types to worry about here, the type of A, the source of
    // the truncate (MidSize), and the destination of the truncate. We know that
    // ASize < MidSize   and MidSize > ResultSize, but don't know the relation
    // between ASize and ResultSize.
    unsigned ASize = A->getType()->getPrimitiveSizeInBits();

    // If the shift amount is larger than the size of A, then the result is
    // known to be zero because all the input bits got shifted out.
    if (Cst->getZExtValue() >= ASize)
      return replaceInstUsesWith(CI, Constant::getNullValue(DestTy));

    // Since we're doing an lshr and a zero extend, and know that the shift
    // amount is smaller than ASize, it is always safe to do the shift in A's
    // type, then zero extend or truncate to the result.
    Value *Shift = Builder->CreateLShr(A, Cst->getZExtValue());
    Shift->takeName(Src);
    return CastInst::CreateIntegerCast(Shift, DestTy, false);
  }

  // Transform trunc(lshr (sext A), Cst) to ashr A, Cst to eliminate type
  // conversion.
  // It works because bits coming from sign extension have the same value as
  // the sign bit of the original value; performing ashr instead of lshr
  // generates bits of the same value as the sign bit.
  if (Src->hasOneUse() &&
      match(Src, m_LShr(m_SExt(m_Value(A)), m_ConstantInt(Cst))) &&
      cast<Instruction>(Src)->getOperand(0)->hasOneUse()) {
    const unsigned ASize = A->getType()->getPrimitiveSizeInBits();
    // This optimization can be only performed when zero bits generated by
    // the original lshr aren't pulled into the value after truncation, so we
    // can only shift by values smaller than the size of destination type (in
    // bits).
    if (Cst->getValue().ult(ASize)) {
      Value *Shift = Builder->CreateAShr(A, Cst->getZExtValue());
      Shift->takeName(Src);
      return CastInst::CreateIntegerCast(Shift, CI.getType(), true);
    }
  }

  if (Instruction *I = shrinkBitwiseLogic(CI))
    return I;

  if (Src->hasOneUse() && isa<IntegerType>(SrcTy) &&
      ShouldChangeType(SrcTy, DestTy)) {
    // Transform "trunc (shl X, cst)" -> "shl (trunc X), cst" so long as the
    // dest type is native and cst < dest size.
    if (match(Src, m_Shl(m_Value(A), m_ConstantInt(Cst))) &&
        !match(A, m_Shr(m_Value(), m_Constant()))) {
      // Skip shifts of shift by constants. It undoes a combine in
      // FoldShiftByConstant and is the extend in reg pattern.
      const unsigned DestSize = DestTy->getScalarSizeInBits();
      if (Cst->getValue().ult(DestSize)) {
        Value *NewTrunc = Builder->CreateTrunc(A, DestTy, A->getName() + ".tr");

        return BinaryOperator::Create(
          Instruction::Shl, NewTrunc,
          ConstantInt::get(DestTy, Cst->getValue().trunc(DestSize)));
      }
    }
  }

  if (Instruction *I = foldVecTruncToExtElt(CI, *this, DL))
    return I;

  return nullptr;
}

Instruction *InstCombiner::transformZExtICmp(ICmpInst *ICI, ZExtInst &CI,
                                             bool DoTransform) {
  // If we are just checking for a icmp eq of a single bit and zext'ing it
  // to an integer, then shift the bit to the appropriate place and then
  // cast to integer to avoid the comparison.
  if (ConstantInt *Op1C = dyn_cast<ConstantInt>(ICI->getOperand(1))) {
    const APInt &Op1CV = Op1C->getValue();

    // zext (x <s  0) to i32 --> x>>u31      true if signbit set.
    // zext (x >s -1) to i32 --> (x>>u31)^1  true if signbit clear.
    if ((ICI->getPredicate() == ICmpInst::ICMP_SLT && Op1CV == 0) ||
        (ICI->getPredicate() == ICmpInst::ICMP_SGT && Op1CV.isAllOnesValue())) {
      if (!DoTransform) return ICI;

      Value *In = ICI->getOperand(0);
      Value *Sh = ConstantInt::get(In->getType(),
                                   In->getType()->getScalarSizeInBits() - 1);
      In = Builder->CreateLShr(In, Sh, In->getName() + ".lobit");
      if (In->getType() != CI.getType())
        In = Builder->CreateIntCast(In, CI.getType(), false/*ZExt*/);

      if (ICI->getPredicate() == ICmpInst::ICMP_SGT) {
        Constant *One = ConstantInt::get(In->getType(), 1);
        In = Builder->CreateXor(In, One, In->getName() + ".not");
      }

      return replaceInstUsesWith(CI, In);
    }

    // zext (X == 0) to i32 --> X^1      iff X has only the low bit set.
    // zext (X == 0) to i32 --> (X>>1)^1 iff X has only the 2nd bit set.
    // zext (X == 1) to i32 --> X        iff X has only the low bit set.
    // zext (X == 2) to i32 --> X>>1     iff X has only the 2nd bit set.
    // zext (X != 0) to i32 --> X        iff X has only the low bit set.
    // zext (X != 0) to i32 --> X>>1     iff X has only the 2nd bit set.
    // zext (X != 1) to i32 --> X^1      iff X has only the low bit set.
    // zext (X != 2) to i32 --> (X>>1)^1 iff X has only the 2nd bit set.
    if ((Op1CV == 0 || Op1CV.isPowerOf2()) &&
        // This only works for EQ and NE
        ICI->isEquality()) {
      // If Op1C some other power of two, convert:
      uint32_t BitWidth = Op1C->getType()->getBitWidth();
      APInt KnownZero(BitWidth, 0), KnownOne(BitWidth, 0);
      computeKnownBits(ICI->getOperand(0), KnownZero, KnownOne, 0, &CI);

      APInt KnownZeroMask(~KnownZero);
      if (KnownZeroMask.isPowerOf2()) { // Exactly 1 possible 1?
        if (!DoTransform) return ICI;

        bool isNE = ICI->getPredicate() == ICmpInst::ICMP_NE;
        if (Op1CV != 0 && (Op1CV != KnownZeroMask)) {
          // (X&4) == 2 --> false
          // (X&4) != 2 --> true
          Constant *Res = ConstantInt::get(Type::getInt1Ty(CI.getContext()),
                                           isNE);
          Res = ConstantExpr::getZExt(Res, CI.getType());
          return replaceInstUsesWith(CI, Res);
        }

        uint32_t ShAmt = KnownZeroMask.logBase2();
        Value *In = ICI->getOperand(0);
        if (ShAmt) {
          // Perform a logical shr by shiftamt.
          // Insert the shift to put the result in the low bit.
          In = Builder->CreateLShr(In, ConstantInt::get(In->getType(), ShAmt),
                                   In->getName() + ".lobit");
        }

        if ((Op1CV != 0) == isNE) { // Toggle the low bit.
          Constant *One = ConstantInt::get(In->getType(), 1);
          In = Builder->CreateXor(In, One);
        }

        if (CI.getType() == In->getType())
          return replaceInstUsesWith(CI, In);

        Value *IntCast = Builder->CreateIntCast(In, CI.getType(), false);
        return replaceInstUsesWith(CI, IntCast);
      }
    }
  }

  // icmp ne A, B is equal to xor A, B when A and B only really have one bit.
  // It is also profitable to transform icmp eq into not(xor(A, B)) because that
  // may lead to additional simplifications.
  if (ICI->isEquality() && CI.getType() == ICI->getOperand(0)->getType()) {
    if (IntegerType *ITy = dyn_cast<IntegerType>(CI.getType())) {
      uint32_t BitWidth = ITy->getBitWidth();
      Value *LHS = ICI->getOperand(0);
      Value *RHS = ICI->getOperand(1);

      APInt KnownZeroLHS(BitWidth, 0), KnownOneLHS(BitWidth, 0);
      APInt KnownZeroRHS(BitWidth, 0), KnownOneRHS(BitWidth, 0);
      computeKnownBits(LHS, KnownZeroLHS, KnownOneLHS, 0, &CI);
      computeKnownBits(RHS, KnownZeroRHS, KnownOneRHS, 0, &CI);

      if (KnownZeroLHS == KnownZeroRHS && KnownOneLHS == KnownOneRHS) {
        APInt KnownBits = KnownZeroLHS | KnownOneLHS;
        APInt UnknownBit = ~KnownBits;
        if (UnknownBit.countPopulation() == 1) {
          if (!DoTransform) return ICI;

          Value *Result = Builder->CreateXor(LHS, RHS);

          // Mask off any bits that are set and won't be shifted away.
          if (KnownOneLHS.uge(UnknownBit))
            Result = Builder->CreateAnd(Result,
                                        ConstantInt::get(ITy, UnknownBit));

          // Shift the bit we're testing down to the lsb.
          Result = Builder->CreateLShr(
               Result, ConstantInt::get(ITy, UnknownBit.countTrailingZeros()));

          if (ICI->getPredicate() == ICmpInst::ICMP_EQ)
            Result = Builder->CreateXor(Result, ConstantInt::get(ITy, 1));
          Result->takeName(ICI);
          return replaceInstUsesWith(CI, Result);
        }
      }
    }
  }

  return nullptr;
}

/// Determine if the specified value can be computed in the specified wider type
/// and produce the same low bits. If not, return false.
///
/// If this function returns true, it can also return a non-zero number of bits
/// (in BitsToClear) which indicates that the value it computes is correct for
/// the zero extend, but that the additional BitsToClear bits need to be zero'd
/// out.  For example, to promote something like:
///
///   %B = trunc i64 %A to i32
///   %C = lshr i32 %B, 8
///   %E = zext i32 %C to i64
///
/// CanEvaluateZExtd for the 'lshr' will return true, and BitsToClear will be
/// set to 8 to indicate that the promoted value needs to have bits 24-31
/// cleared in addition to bits 32-63.  Since an 'and' will be generated to
/// clear the top bits anyway, doing this has no extra cost.
///
/// This function works on both vectors and scalars.
static bool canEvaluateZExtd(Value *V, Type *Ty, unsigned &BitsToClear,
                             InstCombiner &IC, Instruction *CxtI) {
  BitsToClear = 0;
  if (isa<Constant>(V))
    return true;

  Instruction *I = dyn_cast<Instruction>(V);
  if (!I) return false;

  // If the input is a truncate from the destination type, we can trivially
  // eliminate it.
  if (isa<TruncInst>(I) && I->getOperand(0)->getType() == Ty)
    return true;

  // We can't extend or shrink something that has multiple uses: doing so would
  // require duplicating the instruction in general, which isn't profitable.
  if (!I->hasOneUse()) return false;

  unsigned Opc = I->getOpcode(), Tmp;
  switch (Opc) {
  case Instruction::ZExt:  // zext(zext(x)) -> zext(x).
  case Instruction::SExt:  // zext(sext(x)) -> sext(x).
  case Instruction::Trunc: // zext(trunc(x)) -> trunc(x) or zext(x)
    return true;
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
    if (!canEvaluateZExtd(I->getOperand(0), Ty, BitsToClear, IC, CxtI) ||
        !canEvaluateZExtd(I->getOperand(1), Ty, Tmp, IC, CxtI))
      return false;
    // These can all be promoted if neither operand has 'bits to clear'.
    if (BitsToClear == 0 && Tmp == 0)
      return true;

    // If the operation is an AND/OR/XOR and the bits to clear are zero in the
    // other side, BitsToClear is ok.
    if (Tmp == 0 && I->isBitwiseLogicOp()) {
      // We use MaskedValueIsZero here for generality, but the case we care
      // about the most is constant RHS.
      unsigned VSize = V->getType()->getScalarSizeInBits();
      if (IC.MaskedValueIsZero(I->getOperand(1),
                               APInt::getHighBitsSet(VSize, BitsToClear),
                               0, CxtI))
        return true;
    }

    // Otherwise, we don't know how to analyze this BitsToClear case yet.
    return false;

  case Instruction::Shl:
    // We can promote shl(x, cst) if we can promote x.  Since shl overwrites the
    // upper bits we can reduce BitsToClear by the shift amount.
    if (ConstantInt *Amt = dyn_cast<ConstantInt>(I->getOperand(1))) {
      if (!canEvaluateZExtd(I->getOperand(0), Ty, BitsToClear, IC, CxtI))
        return false;
      uint64_t ShiftAmt = Amt->getZExtValue();
      BitsToClear = ShiftAmt < BitsToClear ? BitsToClear - ShiftAmt : 0;
      return true;
    }
    return false;
  case Instruction::LShr:
    // We can promote lshr(x, cst) if we can promote x.  This requires the
    // ultimate 'and' to clear out the high zero bits we're clearing out though.
    if (ConstantInt *Amt = dyn_cast<ConstantInt>(I->getOperand(1))) {
      if (!canEvaluateZExtd(I->getOperand(0), Ty, BitsToClear, IC, CxtI))
        return false;
      BitsToClear += Amt->getZExtValue();
      if (BitsToClear > V->getType()->getScalarSizeInBits())
        BitsToClear = V->getType()->getScalarSizeInBits();
      return true;
    }
    // Cannot promote variable LSHR.
    return false;
  case Instruction::Select:
    if (!canEvaluateZExtd(I->getOperand(1), Ty, Tmp, IC, CxtI) ||
        !canEvaluateZExtd(I->getOperand(2), Ty, BitsToClear, IC, CxtI) ||
        // TODO: If important, we could handle the case when the BitsToClear are
        // known zero in the disagreeing side.
        Tmp != BitsToClear)
      return false;
    return true;

  case Instruction::PHI: {
    // We can change a phi if we can change all operands.  Note that we never
    // get into trouble with cyclic PHIs here because we only consider
    // instructions with a single use.
    PHINode *PN = cast<PHINode>(I);
    if (!canEvaluateZExtd(PN->getIncomingValue(0), Ty, BitsToClear, IC, CxtI))
      return false;
    for (unsigned i = 1, e = PN->getNumIncomingValues(); i != e; ++i)
      if (!canEvaluateZExtd(PN->getIncomingValue(i), Ty, Tmp, IC, CxtI) ||
          // TODO: If important, we could handle the case when the BitsToClear
          // are known zero in the disagreeing input.
          Tmp != BitsToClear)
        return false;
    return true;
  }
  default:
    // TODO: Can handle more cases here.
    return false;
  }
}

Instruction *InstCombiner::visitZExt(ZExtInst &CI) {
  // If this zero extend is only used by a truncate, let the truncate be
  // eliminated before we try to optimize this zext.
  if (CI.hasOneUse() && isa<TruncInst>(CI.user_back()))
    return nullptr;

  // If one of the common conversion will work, do it.
  if (Instruction *Result = commonCastTransforms(CI))
    return Result;

  // See if we can simplify any instructions used by the input whose sole
  // purpose is to compute bits we don't care about.
  if (SimplifyDemandedInstructionBits(CI))
    return &CI;

  Value *Src = CI.getOperand(0);
  Type *SrcTy = Src->getType(), *DestTy = CI.getType();

  // Attempt to extend the entire input expression tree to the destination
  // type.   Only do this if the dest type is a simple type, don't convert the
  // expression tree to something weird like i93 unless the source is also
  // strange.
  unsigned BitsToClear = 0;
  if ((DestTy->isVectorTy() || ShouldChangeType(SrcTy, DestTy)) &&
      canEvaluateZExtd(Src, DestTy, BitsToClear, *this, &CI)) {
    assert(BitsToClear < SrcTy->getScalarSizeInBits() &&
           "Unreasonable BitsToClear");

    // Okay, we can transform this!  Insert the new expression now.
    DEBUG(dbgs() << "ICE: EvaluateInDifferentType converting expression type"
          " to avoid zero extend: " << CI << '\n');
    Value *Res = EvaluateInDifferentType(Src, DestTy, false);
    assert(Res->getType() == DestTy);

    uint32_t SrcBitsKept = SrcTy->getScalarSizeInBits()-BitsToClear;
    uint32_t DestBitSize = DestTy->getScalarSizeInBits();

    // If the high bits are already filled with zeros, just replace this
    // cast with the result.
    if (MaskedValueIsZero(Res,
                          APInt::getHighBitsSet(DestBitSize,
                                                DestBitSize-SrcBitsKept),
                             0, &CI))
      return replaceInstUsesWith(CI, Res);

    // We need to emit an AND to clear the high bits.
    Constant *C = ConstantInt::get(Res->getType(),
                               APInt::getLowBitsSet(DestBitSize, SrcBitsKept));
    return BinaryOperator::CreateAnd(Res, C);
  }

  // If this is a TRUNC followed by a ZEXT then we are dealing with integral
  // types and if the sizes are just right we can convert this into a logical
  // 'and' which will be much cheaper than the pair of casts.
  if (TruncInst *CSrc = dyn_cast<TruncInst>(Src)) {   // A->B->C cast
    // TODO: Subsume this into EvaluateInDifferentType.

    // Get the sizes of the types involved.  We know that the intermediate type
    // will be smaller than A or C, but don't know the relation between A and C.
    Value *A = CSrc->getOperand(0);
    unsigned SrcSize = A->getType()->getScalarSizeInBits();
    unsigned MidSize = CSrc->getType()->getScalarSizeInBits();
    unsigned DstSize = CI.getType()->getScalarSizeInBits();
    // If we're actually extending zero bits, then if
    // SrcSize <  DstSize: zext(a & mask)
    // SrcSize == DstSize: a & mask
    // SrcSize  > DstSize: trunc(a) & mask
    if (SrcSize < DstSize) {
      APInt AndValue(APInt::getLowBitsSet(SrcSize, MidSize));
      Constant *AndConst = ConstantInt::get(A->getType(), AndValue);
      Value *And = Builder->CreateAnd(A, AndConst, CSrc->getName()+".mask");
      return new ZExtInst(And, CI.getType());
    }

    if (SrcSize == DstSize) {
      APInt AndValue(APInt::getLowBitsSet(SrcSize, MidSize));
      return BinaryOperator::CreateAnd(A, ConstantInt::get(A->getType(),
                                                           AndValue));
    }
    if (SrcSize > DstSize) {
      Value *Trunc = Builder->CreateTrunc(A, CI.getType());
      APInt AndValue(APInt::getLowBitsSet(DstSize, MidSize));
      return BinaryOperator::CreateAnd(Trunc,
                                       ConstantInt::get(Trunc->getType(),
                                                        AndValue));
    }
  }

  if (ICmpInst *ICI = dyn_cast<ICmpInst>(Src))
    return transformZExtICmp(ICI, CI);

  BinaryOperator *SrcI = dyn_cast<BinaryOperator>(Src);
  if (SrcI && SrcI->getOpcode() == Instruction::Or) {
    // zext (or icmp, icmp) -> or (zext icmp), (zext icmp) if at least one
    // of the (zext icmp) can be eliminated. If so, immediately perform the
    // according elimination.
    ICmpInst *LHS = dyn_cast<ICmpInst>(SrcI->getOperand(0));
    ICmpInst *RHS = dyn_cast<ICmpInst>(SrcI->getOperand(1));
    if (LHS && RHS && LHS->hasOneUse() && RHS->hasOneUse() &&
        (transformZExtICmp(LHS, CI, false) ||
         transformZExtICmp(RHS, CI, false))) {
      // zext (or icmp, icmp) -> or (zext icmp), (zext icmp)
      Value *LCast = Builder->CreateZExt(LHS, CI.getType(), LHS->getName());
      Value *RCast = Builder->CreateZExt(RHS, CI.getType(), RHS->getName());
      BinaryOperator *Or = BinaryOperator::Create(Instruction::Or, LCast, RCast);

      // Perform the elimination.
      if (auto *LZExt = dyn_cast<ZExtInst>(LCast))
        transformZExtICmp(LHS, *LZExt);
      if (auto *RZExt = dyn_cast<ZExtInst>(RCast))
        transformZExtICmp(RHS, *RZExt);

      return Or;
    }
  }

  // zext(trunc(X) & C) -> (X & zext(C)).
  Constant *C = nullptr;
  Value *X = nullptr;
  if (SrcI &&
      match(SrcI, m_OneUse(m_And(m_Trunc(m_Value(X)), m_Constant(C)))) &&
      X->getType() == CI.getType())
    return BinaryOperator::CreateAnd(X, ConstantExpr::getZExt(C, CI.getType()));

  // zext((trunc(X) & C) ^ C) -> ((X & zext(C)) ^ zext(C)).
  Value *And;
  if (SrcI && match(SrcI, m_OneUse(m_Xor(m_Value(And), m_Constant(C)))) &&
      match(And, m_OneUse(m_And(m_Trunc(m_Value(X)), m_Specific(C)))) &&
      X->getType() == CI.getType()) {
    Constant *ZC = ConstantExpr::getZExt(C, CI.getType());
    return BinaryOperator::CreateXor(Builder->CreateAnd(X, ZC), ZC);
  }

  return nullptr;
}

/// Transform (sext icmp) to bitwise / integer operations to eliminate the icmp.
Instruction *InstCombiner::transformSExtICmp(ICmpInst *ICI, Instruction &CI) {
  Value *Op0 = ICI->getOperand(0), *Op1 = ICI->getOperand(1);
  ICmpInst::Predicate Pred = ICI->getPredicate();

  // Don't bother if Op1 isn't of vector or integer type.
  if (!Op1->getType()->isIntOrIntVectorTy())
    return nullptr;

  if (Constant *Op1C = dyn_cast<Constant>(Op1)) {
    // (x <s  0) ? -1 : 0 -> ashr x, 31        -> all ones if negative
    // (x >s -1) ? -1 : 0 -> not (ashr x, 31)  -> all ones if positive
    if ((Pred == ICmpInst::ICMP_SLT && Op1C->isNullValue()) ||
        (Pred == ICmpInst::ICMP_SGT && Op1C->isAllOnesValue())) {

      Value *Sh = ConstantInt::get(Op0->getType(),
                                   Op0->getType()->getScalarSizeInBits()-1);
      Value *In = Builder->CreateAShr(Op0, Sh, Op0->getName()+".lobit");
      if (In->getType() != CI.getType())
        In = Builder->CreateIntCast(In, CI.getType(), true/*SExt*/);

      if (Pred == ICmpInst::ICMP_SGT)
        In = Builder->CreateNot(In, In->getName()+".not");
      return replaceInstUsesWith(CI, In);
    }
  }

  if (ConstantInt *Op1C = dyn_cast<ConstantInt>(Op1)) {
    // If we know that only one bit of the LHS of the icmp can be set and we
    // have an equality comparison with zero or a power of 2, we can transform
    // the icmp and sext into bitwise/integer operations.
    if (ICI->hasOneUse() &&
        ICI->isEquality() && (Op1C->isZero() || Op1C->getValue().isPowerOf2())){
      unsigned BitWidth = Op1C->getType()->getBitWidth();
      APInt KnownZero(BitWidth, 0), KnownOne(BitWidth, 0);
      computeKnownBits(Op0, KnownZero, KnownOne, 0, &CI);

      APInt KnownZeroMask(~KnownZero);
      if (KnownZeroMask.isPowerOf2()) {
        Value *In = ICI->getOperand(0);

        // If the icmp tests for a known zero bit we can constant fold it.
        if (!Op1C->isZero() && Op1C->getValue() != KnownZeroMask) {
          Value *V = Pred == ICmpInst::ICMP_NE ?
                       ConstantInt::getAllOnesValue(CI.getType()) :
                       ConstantInt::getNullValue(CI.getType());
          return replaceInstUsesWith(CI, V);
        }

        if (!Op1C->isZero() == (Pred == ICmpInst::ICMP_NE)) {
          // sext ((x & 2^n) == 0)   -> (x >> n) - 1
          // sext ((x & 2^n) != 2^n) -> (x >> n) - 1
          unsigned ShiftAmt = KnownZeroMask.countTrailingZeros();
          // Perform a right shift to place the desired bit in the LSB.
          if (ShiftAmt)
            In = Builder->CreateLShr(In,
                                     ConstantInt::get(In->getType(), ShiftAmt));

          // At this point "In" is either 1 or 0. Subtract 1 to turn
          // {1, 0} -> {0, -1}.
          In = Builder->CreateAdd(In,
                                  ConstantInt::getAllOnesValue(In->getType()),
                                  "sext");
        } else {
          // sext ((x & 2^n) != 0)   -> (x << bitwidth-n) a>> bitwidth-1
          // sext ((x & 2^n) == 2^n) -> (x << bitwidth-n) a>> bitwidth-1
          unsigned ShiftAmt = KnownZeroMask.countLeadingZeros();
          // Perform a left shift to place the desired bit in the MSB.
          if (ShiftAmt)
            In = Builder->CreateShl(In,
                                    ConstantInt::get(In->getType(), ShiftAmt));

          // Distribute the bit over the whole bit width.
          In = Builder->CreateAShr(In, ConstantInt::get(In->getType(),
                                                        BitWidth - 1), "sext");
        }

        if (CI.getType() == In->getType())
          return replaceInstUsesWith(CI, In);
        return CastInst::CreateIntegerCast(In, CI.getType(), true/*SExt*/);
      }
    }
  }

  return nullptr;
}

/// Return true if we can take the specified value and return it as type Ty
/// without inserting any new casts and without changing the value of the common
/// low bits.  This is used by code that tries to promote integer operations to
/// a wider types will allow us to eliminate the extension.
///
/// This function works on both vectors and scalars.
///
static bool canEvaluateSExtd(Value *V, Type *Ty) {
  assert(V->getType()->getScalarSizeInBits() < Ty->getScalarSizeInBits() &&
         "Can't sign extend type to a smaller type");
  // If this is a constant, it can be trivially promoted.
  if (isa<Constant>(V))
    return true;

  Instruction *I = dyn_cast<Instruction>(V);
  if (!I) return false;

  // If this is a truncate from the dest type, we can trivially eliminate it.
  if (isa<TruncInst>(I) && I->getOperand(0)->getType() == Ty)
    return true;

  // We can't extend or shrink something that has multiple uses: doing so would
  // require duplicating the instruction in general, which isn't profitable.
  if (!I->hasOneUse()) return false;

  switch (I->getOpcode()) {
  case Instruction::SExt:  // sext(sext(x)) -> sext(x)
  case Instruction::ZExt:  // sext(zext(x)) -> zext(x)
  case Instruction::Trunc: // sext(trunc(x)) -> trunc(x) or sext(x)
    return true;
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
    // These operators can all arbitrarily be extended if their inputs can.
    return canEvaluateSExtd(I->getOperand(0), Ty) &&
           canEvaluateSExtd(I->getOperand(1), Ty);

  //case Instruction::Shl:   TODO
  //case Instruction::LShr:  TODO

  case Instruction::Select:
    return canEvaluateSExtd(I->getOperand(1), Ty) &&
           canEvaluateSExtd(I->getOperand(2), Ty);

  case Instruction::PHI: {
    // We can change a phi if we can change all operands.  Note that we never
    // get into trouble with cyclic PHIs here because we only consider
    // instructions with a single use.
    PHINode *PN = cast<PHINode>(I);
    for (Value *IncValue : PN->incoming_values())
      if (!canEvaluateSExtd(IncValue, Ty)) return false;
    return true;
  }
  default:
    // TODO: Can handle more cases here.
    break;
  }

  return false;
}

Instruction *InstCombiner::visitSExt(SExtInst &CI) {
  // If this sign extend is only used by a truncate, let the truncate be
  // eliminated before we try to optimize this sext.
  if (CI.hasOneUse() && isa<TruncInst>(CI.user_back()))
    return nullptr;

  if (Instruction *I = commonCastTransforms(CI))
    return I;

  // See if we can simplify any instructions used by the input whose sole
  // purpose is to compute bits we don't care about.
  if (SimplifyDemandedInstructionBits(CI))
    return &CI;

  Value *Src = CI.getOperand(0);
  Type *SrcTy = Src->getType(), *DestTy = CI.getType();

  // If we know that the value being extended is positive, we can use a zext
  // instead.
  bool KnownZero = false, KnownOne = false;
  ComputeSignBit(Src, KnownZero, KnownOne, 0, &CI);
  if (KnownZero) {
    Value *ZExt = Builder->CreateZExt(Src, DestTy);
    return replaceInstUsesWith(CI, ZExt);
  }

  // Attempt to extend the entire input expression tree to the destination
  // type.   Only do this if the dest type is a simple type, don't convert the
  // expression tree to something weird like i93 unless the source is also
  // strange.
  if ((DestTy->isVectorTy() || ShouldChangeType(SrcTy, DestTy)) &&
      canEvaluateSExtd(Src, DestTy)) {
    // Okay, we can transform this!  Insert the new expression now.
    DEBUG(dbgs() << "ICE: EvaluateInDifferentType converting expression type"
          " to avoid sign extend: " << CI << '\n');
    Value *Res = EvaluateInDifferentType(Src, DestTy, true);
    assert(Res->getType() == DestTy);

    uint32_t SrcBitSize = SrcTy->getScalarSizeInBits();
    uint32_t DestBitSize = DestTy->getScalarSizeInBits();

    // If the high bits are already filled with sign bit, just replace this
    // cast with the result.
    if (ComputeNumSignBits(Res, 0, &CI) > DestBitSize - SrcBitSize)
      return replaceInstUsesWith(CI, Res);

    // We need to emit a shl + ashr to do the sign extend.
    Value *ShAmt = ConstantInt::get(DestTy, DestBitSize-SrcBitSize);
    return BinaryOperator::CreateAShr(Builder->CreateShl(Res, ShAmt, "sext"),
                                      ShAmt);
  }

  // If this input is a trunc from our destination, then turn sext(trunc(x))
  // into shifts.
  if (TruncInst *TI = dyn_cast<TruncInst>(Src))
    if (TI->hasOneUse() && TI->getOperand(0)->getType() == DestTy) {
      uint32_t SrcBitSize = SrcTy->getScalarSizeInBits();
      uint32_t DestBitSize = DestTy->getScalarSizeInBits();

      // We need to emit a shl + ashr to do the sign extend.
      Value *ShAmt = ConstantInt::get(DestTy, DestBitSize-SrcBitSize);
      Value *Res = Builder->CreateShl(TI->getOperand(0), ShAmt, "sext");
      return BinaryOperator::CreateAShr(Res, ShAmt);
    }

  if (ICmpInst *ICI = dyn_cast<ICmpInst>(Src))
    return transformSExtICmp(ICI, CI);

  // If the input is a shl/ashr pair of a same constant, then this is a sign
  // extension from a smaller value.  If we could trust arbitrary bitwidth
  // integers, we could turn this into a truncate to the smaller bit and then
  // use a sext for the whole extension.  Since we don't, look deeper and check
  // for a truncate.  If the source and dest are the same type, eliminate the
  // trunc and extend and just do shifts.  For example, turn:
  //   %a = trunc i32 %i to i8
  //   %b = shl i8 %a, 6
  //   %c = ashr i8 %b, 6
  //   %d = sext i8 %c to i32
  // into:
  //   %a = shl i32 %i, 30
  //   %d = ashr i32 %a, 30
  Value *A = nullptr;
  // TODO: Eventually this could be subsumed by EvaluateInDifferentType.
  ConstantInt *BA = nullptr, *CA = nullptr;
  if (match(Src, m_AShr(m_Shl(m_Trunc(m_Value(A)), m_ConstantInt(BA)),
                        m_ConstantInt(CA))) &&
      BA == CA && A->getType() == CI.getType()) {
    unsigned MidSize = Src->getType()->getScalarSizeInBits();
    unsigned SrcDstSize = CI.getType()->getScalarSizeInBits();
    unsigned ShAmt = CA->getZExtValue()+SrcDstSize-MidSize;
    Constant *ShAmtV = ConstantInt::get(CI.getType(), ShAmt);
    A = Builder->CreateShl(A, ShAmtV, CI.getName());
    return BinaryOperator::CreateAShr(A, ShAmtV);
  }

  return nullptr;
}


/// Return a Constant* for the specified floating-point constant if it fits
/// in the specified FP type without changing its value.
static Constant *fitsInFPType(ConstantFP *CFP, const fltSemantics &Sem) {
  bool losesInfo = false;
  APFloat F = CFP->getValueAPF();
  (void)F.convert(Sem, APFloat::rmNearestTiesToEven, &losesInfo);
  if (!losesInfo)
    return ConstantFP::get(CFP->getContext(), F);
  return nullptr;
}

/// If this is a floating-point extension instruction, look
/// through it until we get the source value.
static Value *lookThroughFPExtensions(Value *V) {
  if (Instruction *I = dyn_cast<Instruction>(V))
    if (I->getOpcode() == Instruction::FPExt)
      return lookThroughFPExtensions(I->getOperand(0));

  // If this value is a constant, return the constant in the smallest FP type
  // that can accurately represent it.  This allows us to turn
  // (float)((double)X+2.0) into x+2.0f.
  if (ConstantFP *CFP = dyn_cast<ConstantFP>(V)) {
    if (CFP->getType() == Type::getPPC_FP128Ty(V->getContext()))
      return V;  // No constant folding of this.
    // See if the value can be truncated to half and then reextended.
    if (Value *V = fitsInFPType(CFP, APFloat::IEEEhalf()))
      return V;
    // See if the value can be truncated to float and then reextended.
    if (Value *V = fitsInFPType(CFP, APFloat::IEEEsingle()))
      return V;
    if (CFP->getType()->isDoubleTy())
      return V;  // Won't shrink.
    if (Value *V = fitsInFPType(CFP, APFloat::IEEEdouble()))
      return V;
    // Don't try to shrink to various long double types.
  }

  return V;
}

Instruction *InstCombiner::visitFPTrunc(FPTruncInst &CI) {
  if (Instruction *I = commonCastTransforms(CI))
    return I;
  // If we have fptrunc(OpI (fpextend x), (fpextend y)), we would like to
  // simplify this expression to avoid one or more of the trunc/extend
  // operations if we can do so without changing the numerical results.
  //
  // The exact manner in which the widths of the operands interact to limit
  // what we can and cannot do safely varies from operation to operation, and
  // is explained below in the various case statements.
  BinaryOperator *OpI = dyn_cast<BinaryOperator>(CI.getOperand(0));
  if (OpI && OpI->hasOneUse()) {
    Value *LHSOrig = lookThroughFPExtensions(OpI->getOperand(0));
    Value *RHSOrig = lookThroughFPExtensions(OpI->getOperand(1));
    unsigned OpWidth = OpI->getType()->getFPMantissaWidth();
    unsigned LHSWidth = LHSOrig->getType()->getFPMantissaWidth();
    unsigned RHSWidth = RHSOrig->getType()->getFPMantissaWidth();
    unsigned SrcWidth = std::max(LHSWidth, RHSWidth);
    unsigned DstWidth = CI.getType()->getFPMantissaWidth();
    switch (OpI->getOpcode()) {
      default: break;
      case Instruction::FAdd:
      case Instruction::FSub:
        // For addition and subtraction, the infinitely precise result can
        // essentially be arbitrarily wide; proving that double rounding
        // will not occur because the result of OpI is exact (as we will for
        // FMul, for example) is hopeless.  However, we *can* nonetheless
        // frequently know that double rounding cannot occur (or that it is
        // innocuous) by taking advantage of the specific structure of
        // infinitely-precise results that admit double rounding.
        //
        // Specifically, if OpWidth >= 2*DstWdith+1 and DstWidth is sufficient
        // to represent both sources, we can guarantee that the double
        // rounding is innocuous (See p50 of Figueroa's 2000 PhD thesis,
        // "A Rigorous Framework for Fully Supporting the IEEE Standard ..."
        // for proof of this fact).
        //
        // Note: Figueroa does not consider the case where DstFormat !=
        // SrcFormat.  It's possible (likely even!) that this analysis
        // could be tightened for those cases, but they are rare (the main
        // case of interest here is (float)((double)float + float)).
        if (OpWidth >= 2*DstWidth+1 && DstWidth >= SrcWidth) {
          if (LHSOrig->getType() != CI.getType())
            LHSOrig = Builder->CreateFPExt(LHSOrig, CI.getType());
          if (RHSOrig->getType() != CI.getType())
            RHSOrig = Builder->CreateFPExt(RHSOrig, CI.getType());
          Instruction *RI =
            BinaryOperator::Create(OpI->getOpcode(), LHSOrig, RHSOrig);
          RI->copyFastMathFlags(OpI);
          return RI;
        }
        break;
      case Instruction::FMul:
        // For multiplication, the infinitely precise result has at most
        // LHSWidth + RHSWidth significant bits; if OpWidth is sufficient
        // that such a value can be exactly represented, then no double
        // rounding can possibly occur; we can safely perform the operation
        // in the destination format if it can represent both sources.
        if (OpWidth >= LHSWidth + RHSWidth && DstWidth >= SrcWidth) {
          if (LHSOrig->getType() != CI.getType())
            LHSOrig = Builder->CreateFPExt(LHSOrig, CI.getType());
          if (RHSOrig->getType() != CI.getType())
            RHSOrig = Builder->CreateFPExt(RHSOrig, CI.getType());
          Instruction *RI =
            BinaryOperator::CreateFMul(LHSOrig, RHSOrig);
          RI->copyFastMathFlags(OpI);
          return RI;
        }
        break;
      case Instruction::FDiv:
        // For division, we use again use the bound from Figueroa's
        // dissertation.  I am entirely certain that this bound can be
        // tightened in the unbalanced operand case by an analysis based on
        // the diophantine rational approximation bound, but the well-known
        // condition used here is a good conservative first pass.
        // TODO: Tighten bound via rigorous analysis of the unbalanced case.
        if (OpWidth >= 2*DstWidth && DstWidth >= SrcWidth) {
          if (LHSOrig->getType() != CI.getType())
            LHSOrig = Builder->CreateFPExt(LHSOrig, CI.getType());
          if (RHSOrig->getType() != CI.getType())
            RHSOrig = Builder->CreateFPExt(RHSOrig, CI.getType());
          Instruction *RI =
            BinaryOperator::CreateFDiv(LHSOrig, RHSOrig);
          RI->copyFastMathFlags(OpI);
          return RI;
        }
        break;
      case Instruction::FRem:
        // Remainder is straightforward.  Remainder is always exact, so the
        // type of OpI doesn't enter into things at all.  We simply evaluate
        // in whichever source type is larger, then convert to the
        // destination type.
        if (SrcWidth == OpWidth)
          break;
        if (LHSWidth < SrcWidth)
          LHSOrig = Builder->CreateFPExt(LHSOrig, RHSOrig->getType());
        else if (RHSWidth <= SrcWidth)
          RHSOrig = Builder->CreateFPExt(RHSOrig, LHSOrig->getType());
        if (LHSOrig != OpI->getOperand(0) || RHSOrig != OpI->getOperand(1)) {
          Value *ExactResult = Builder->CreateFRem(LHSOrig, RHSOrig);
          if (Instruction *RI = dyn_cast<Instruction>(ExactResult))
            RI->copyFastMathFlags(OpI);
          return CastInst::CreateFPCast(ExactResult, CI.getType());
        }
    }

    // (fptrunc (fneg x)) -> (fneg (fptrunc x))
    if (BinaryOperator::isFNeg(OpI)) {
      Value *InnerTrunc = Builder->CreateFPTrunc(OpI->getOperand(1),
                                                 CI.getType());
      Instruction *RI = BinaryOperator::CreateFNeg(InnerTrunc);
      RI->copyFastMathFlags(OpI);
      return RI;
    }
  }

  // (fptrunc (select cond, R1, Cst)) -->
  // (select cond, (fptrunc R1), (fptrunc Cst))
  //
  //  - but only if this isn't part of a min/max operation, else we'll
  // ruin min/max canonical form which is to have the select and
  // compare's operands be of the same type with no casts to look through.
  Value *LHS = nullptr, *RHS = nullptr;
  SelectInst *SI = dyn_cast<SelectInst>(CI.getOperand(0));
  if (SI &&
      (isa<ConstantFP>(SI->getOperand(1)) ||
       isa<ConstantFP>(SI->getOperand(2))) &&
      matchSelectPattern(SI, LHS, RHS).Flavor == SPF_UNKNOWN) {
    Value *LHSTrunc = Builder->CreateFPTrunc(SI->getOperand(1),
                                             CI.getType());
    Value *RHSTrunc = Builder->CreateFPTrunc(SI->getOperand(2),
                                             CI.getType());
    return SelectInst::Create(SI->getOperand(0), LHSTrunc, RHSTrunc);
  }

  IntrinsicInst *II = dyn_cast<IntrinsicInst>(CI.getOperand(0));
  if (II) {
    switch (II->getIntrinsicID()) {
      default: break;
      case Intrinsic::fabs: {
        // (fptrunc (fabs x)) -> (fabs (fptrunc x))
        Value *InnerTrunc = Builder->CreateFPTrunc(II->getArgOperand(0),
                                                   CI.getType());
        Type *IntrinsicType[] = { CI.getType() };
        Function *Overload = Intrinsic::getDeclaration(
            CI.getModule(), II->getIntrinsicID(), IntrinsicType);

        SmallVector<OperandBundleDef, 1> OpBundles;
        II->getOperandBundlesAsDefs(OpBundles);

        Value *Args[] = { InnerTrunc };
        return CallInst::Create(Overload, Args, OpBundles, II->getName());
      }
    }
  }

  return nullptr;
}

Instruction *InstCombiner::visitFPExt(CastInst &CI) {
  return commonCastTransforms(CI);
}

// fpto{s/u}i({u/s}itofp(X)) --> X or zext(X) or sext(X) or trunc(X)
// This is safe if the intermediate type has enough bits in its mantissa to
// accurately represent all values of X.  For example, this won't work with
// i64 -> float -> i64.
Instruction *InstCombiner::FoldItoFPtoI(Instruction &FI) {
  if (!isa<UIToFPInst>(FI.getOperand(0)) && !isa<SIToFPInst>(FI.getOperand(0)))
    return nullptr;
  Instruction *OpI = cast<Instruction>(FI.getOperand(0));

  Value *SrcI = OpI->getOperand(0);
  Type *FITy = FI.getType();
  Type *OpITy = OpI->getType();
  Type *SrcTy = SrcI->getType();
  bool IsInputSigned = isa<SIToFPInst>(OpI);
  bool IsOutputSigned = isa<FPToSIInst>(FI);

  // We can safely assume the conversion won't overflow the output range,
  // because (for example) (uint8_t)18293.f is undefined behavior.

  // Since we can assume the conversion won't overflow, our decision as to
  // whether the input will fit in the float should depend on the minimum
  // of the input range and output range.

  // This means this is also safe for a signed input and unsigned output, since
  // a negative input would lead to undefined behavior.
  int InputSize = (int)SrcTy->getScalarSizeInBits() - IsInputSigned;
  int OutputSize = (int)FITy->getScalarSizeInBits() - IsOutputSigned;
  int ActualSize = std::min(InputSize, OutputSize);

  if (ActualSize <= OpITy->getFPMantissaWidth()) {
    if (FITy->getScalarSizeInBits() > SrcTy->getScalarSizeInBits()) {
      if (IsInputSigned && IsOutputSigned)
        return new SExtInst(SrcI, FITy);
      return new ZExtInst(SrcI, FITy);
    }
    if (FITy->getScalarSizeInBits() < SrcTy->getScalarSizeInBits())
      return new TruncInst(SrcI, FITy);
    if (SrcTy == FITy)
      return replaceInstUsesWith(FI, SrcI);
    return new BitCastInst(SrcI, FITy);
  }
  return nullptr;
}

Instruction *InstCombiner::visitFPToUI(FPToUIInst &FI) {
  Instruction *OpI = dyn_cast<Instruction>(FI.getOperand(0));
  if (!OpI)
    return commonCastTransforms(FI);

  if (Instruction *I = FoldItoFPtoI(FI))
    return I;

  return commonCastTransforms(FI);
}

Instruction *InstCombiner::visitFPToSI(FPToSIInst &FI) {
  Instruction *OpI = dyn_cast<Instruction>(FI.getOperand(0));
  if (!OpI)
    return commonCastTransforms(FI);

  if (Instruction *I = FoldItoFPtoI(FI))
    return I;

  return commonCastTransforms(FI);
}

Instruction *InstCombiner::visitUIToFP(CastInst &CI) {
  return commonCastTransforms(CI);
}

Instruction *InstCombiner::visitSIToFP(CastInst &CI) {
  return commonCastTransforms(CI);
}

Instruction *InstCombiner::visitIntToPtr(IntToPtrInst &CI) {
  // If the source integer type is not the intptr_t type for this target, do a
  // trunc or zext to the intptr_t type, then inttoptr of it.  This allows the
  // cast to be exposed to other transforms.
  unsigned AS = CI.getAddressSpace();
  if (CI.getOperand(0)->getType()->getScalarSizeInBits() !=
      DL.getPointerSizeInBits(AS)) {
    Type *Ty = DL.getIntPtrType(CI.getContext(), AS);
    if (CI.getType()->isVectorTy()) // Handle vectors of pointers.
      Ty = VectorType::get(Ty, CI.getType()->getVectorNumElements());

    Value *P = Builder->CreateZExtOrTrunc(CI.getOperand(0), Ty);
    return new IntToPtrInst(P, CI.getType());
  }

  if (Instruction *I = commonCastTransforms(CI))
    return I;

  return nullptr;
}

/// @brief Implement the transforms for cast of pointer (bitcast/ptrtoint)
Instruction *InstCombiner::commonPointerCastTransforms(CastInst &CI) {
  Value *Src = CI.getOperand(0);

  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Src)) {
    // If casting the result of a getelementptr instruction with no offset, turn
    // this into a cast of the original pointer!
    if (GEP->hasAllZeroIndices() &&
        // If CI is an addrspacecast and GEP changes the poiner type, merging
        // GEP into CI would undo canonicalizing addrspacecast with different
        // pointer types, causing infinite loops.
        (!isa<AddrSpaceCastInst>(CI) ||
          GEP->getType() == GEP->getPointerOperand()->getType())) {
      // Changing the cast operand is usually not a good idea but it is safe
      // here because the pointer operand is being replaced with another
      // pointer operand so the opcode doesn't need to change.
      Worklist.Add(GEP);
      CI.setOperand(0, GEP->getOperand(0));
      return &CI;
    }
  }

  return commonCastTransforms(CI);
}

Instruction *InstCombiner::visitPtrToInt(PtrToIntInst &CI) {
  // If the destination integer type is not the intptr_t type for this target,
  // do a ptrtoint to intptr_t then do a trunc or zext.  This allows the cast
  // to be exposed to other transforms.

  Type *Ty = CI.getType();
  unsigned AS = CI.getPointerAddressSpace();

  if (Ty->getScalarSizeInBits() == DL.getPointerSizeInBits(AS))
    return commonPointerCastTransforms(CI);

  Type *PtrTy = DL.getIntPtrType(CI.getContext(), AS);
  if (Ty->isVectorTy()) // Handle vectors of pointers.
    PtrTy = VectorType::get(PtrTy, Ty->getVectorNumElements());

  Value *P = Builder->CreatePtrToInt(CI.getOperand(0), PtrTy);
  return CastInst::CreateIntegerCast(P, Ty, /*isSigned=*/false);
}

/// This input value (which is known to have vector type) is being zero extended
/// or truncated to the specified vector type.
/// Try to replace it with a shuffle (and vector/vector bitcast) if possible.
///
/// The source and destination vector types may have different element types.
static Instruction *optimizeVectorResize(Value *InVal, VectorType *DestTy,
                                         InstCombiner &IC) {
  // We can only do this optimization if the output is a multiple of the input
  // element size, or the input is a multiple of the output element size.
  // Convert the input type to have the same element type as the output.
  VectorType *SrcTy = cast<VectorType>(InVal->getType());

  if (SrcTy->getElementType() != DestTy->getElementType()) {
    // The input types don't need to be identical, but for now they must be the
    // same size.  There is no specific reason we couldn't handle things like
    // <4 x i16> -> <4 x i32> by bitcasting to <2 x i32> but haven't gotten
    // there yet.
    if (SrcTy->getElementType()->getPrimitiveSizeInBits() !=
        DestTy->getElementType()->getPrimitiveSizeInBits())
      return nullptr;

    SrcTy = VectorType::get(DestTy->getElementType(), SrcTy->getNumElements());
    InVal = IC.Builder->CreateBitCast(InVal, SrcTy);
  }

  // Now that the element types match, get the shuffle mask and RHS of the
  // shuffle to use, which depends on whether we're increasing or decreasing the
  // size of the input.
  SmallVector<uint32_t, 16> ShuffleMask;
  Value *V2 = nullptr;

  if (SrcTy->getNumElements() > DestTy->getNumElements()) {
    // If we're shrinking the number of elements, just shuffle in the low
    // elements from the input and use undef as the second shuffle input.
    V2 = UndefValue::get(SrcTy);
    for (unsigned i = 0, e = DestTy->getNumElements(); i != e; ++i)
      ShuffleMask.push_back(i);

  } else {
    // If we're increasing the number of elements, shuffle in all of the
    // elements from InVal and fill the rest of the result elements with zeros
    // from a constant zero.
    V2 = Constant::getNullValue(SrcTy);
    unsigned SrcElts = SrcTy->getNumElements();
    for (unsigned i = 0, e = SrcElts; i != e; ++i)
      ShuffleMask.push_back(i);

    // The excess elements reference the first element of the zero input.
    for (unsigned i = 0, e = DestTy->getNumElements()-SrcElts; i != e; ++i)
      ShuffleMask.push_back(SrcElts);
  }

  return new ShuffleVectorInst(InVal, V2,
                               ConstantDataVector::get(V2->getContext(),
                                                       ShuffleMask));
}

static bool isMultipleOfTypeSize(unsigned Value, Type *Ty) {
  return Value % Ty->getPrimitiveSizeInBits() == 0;
}

static unsigned getTypeSizeIndex(unsigned Value, Type *Ty) {
  return Value / Ty->getPrimitiveSizeInBits();
}

/// V is a value which is inserted into a vector of VecEltTy.
/// Look through the value to see if we can decompose it into
/// insertions into the vector.  See the example in the comment for
/// OptimizeIntegerToVectorInsertions for the pattern this handles.
/// The type of V is always a non-zero multiple of VecEltTy's size.
/// Shift is the number of bits between the lsb of V and the lsb of
/// the vector.
///
/// This returns false if the pattern can't be matched or true if it can,
/// filling in Elements with the elements found here.
static bool collectInsertionElements(Value *V, unsigned Shift,
                                     SmallVectorImpl<Value *> &Elements,
                                     Type *VecEltTy, bool isBigEndian) {
  assert(isMultipleOfTypeSize(Shift, VecEltTy) &&
         "Shift should be a multiple of the element type size");

  // Undef values never contribute useful bits to the result.
  if (isa<UndefValue>(V)) return true;

  // If we got down to a value of the right type, we win, try inserting into the
  // right element.
  if (V->getType() == VecEltTy) {
    // Inserting null doesn't actually insert any elements.
    if (Constant *C = dyn_cast<Constant>(V))
      if (C->isNullValue())
        return true;

    unsigned ElementIndex = getTypeSizeIndex(Shift, VecEltTy);
    if (isBigEndian)
      ElementIndex = Elements.size() - ElementIndex - 1;

    // Fail if multiple elements are inserted into this slot.
    if (Elements[ElementIndex])
      return false;

    Elements[ElementIndex] = V;
    return true;
  }

  if (Constant *C = dyn_cast<Constant>(V)) {
    // Figure out the # elements this provides, and bitcast it or slice it up
    // as required.
    unsigned NumElts = getTypeSizeIndex(C->getType()->getPrimitiveSizeInBits(),
                                        VecEltTy);
    // If the constant is the size of a vector element, we just need to bitcast
    // it to the right type so it gets properly inserted.
    if (NumElts == 1)
      return collectInsertionElements(ConstantExpr::getBitCast(C, VecEltTy),
                                      Shift, Elements, VecEltTy, isBigEndian);

    // Okay, this is a constant that covers multiple elements.  Slice it up into
    // pieces and insert each element-sized piece into the vector.
    if (!isa<IntegerType>(C->getType()))
      C = ConstantExpr::getBitCast(C, IntegerType::get(V->getContext(),
                                       C->getType()->getPrimitiveSizeInBits()));
    unsigned ElementSize = VecEltTy->getPrimitiveSizeInBits();
    Type *ElementIntTy = IntegerType::get(C->getContext(), ElementSize);

    for (unsigned i = 0; i != NumElts; ++i) {
      unsigned ShiftI = Shift+i*ElementSize;
      Constant *Piece = ConstantExpr::getLShr(C, ConstantInt::get(C->getType(),
                                                                  ShiftI));
      Piece = ConstantExpr::getTrunc(Piece, ElementIntTy);
      if (!collectInsertionElements(Piece, ShiftI, Elements, VecEltTy,
                                    isBigEndian))
        return false;
    }
    return true;
  }

  if (!V->hasOneUse()) return false;

  Instruction *I = dyn_cast<Instruction>(V);
  if (!I) return false;
  switch (I->getOpcode()) {
  default: return false; // Unhandled case.
  case Instruction::BitCast:
    return collectInsertionElements(I->getOperand(0), Shift, Elements, VecEltTy,
                                    isBigEndian);
  case Instruction::ZExt:
    if (!isMultipleOfTypeSize(
                          I->getOperand(0)->getType()->getPrimitiveSizeInBits(),
                              VecEltTy))
      return false;
    return collectInsertionElements(I->getOperand(0), Shift, Elements, VecEltTy,
                                    isBigEndian);
  case Instruction::Or:
    return collectInsertionElements(I->getOperand(0), Shift, Elements, VecEltTy,
                                    isBigEndian) &&
           collectInsertionElements(I->getOperand(1), Shift, Elements, VecEltTy,
                                    isBigEndian);
  case Instruction::Shl: {
    // Must be shifting by a constant that is a multiple of the element size.
    ConstantInt *CI = dyn_cast<ConstantInt>(I->getOperand(1));
    if (!CI) return false;
    Shift += CI->getZExtValue();
    if (!isMultipleOfTypeSize(Shift, VecEltTy)) return false;
    return collectInsertionElements(I->getOperand(0), Shift, Elements, VecEltTy,
                                    isBigEndian);
  }

  }
}


/// If the input is an 'or' instruction, we may be doing shifts and ors to
/// assemble the elements of the vector manually.
/// Try to rip the code out and replace it with insertelements.  This is to
/// optimize code like this:
///
///    %tmp37 = bitcast float %inc to i32
///    %tmp38 = zext i32 %tmp37 to i64
///    %tmp31 = bitcast float %inc5 to i32
///    %tmp32 = zext i32 %tmp31 to i64
///    %tmp33 = shl i64 %tmp32, 32
///    %ins35 = or i64 %tmp33, %tmp38
///    %tmp43 = bitcast i64 %ins35 to <2 x float>
///
/// Into two insertelements that do "buildvector{%inc, %inc5}".
static Value *optimizeIntegerToVectorInsertions(BitCastInst &CI,
                                                InstCombiner &IC) {
  VectorType *DestVecTy = cast<VectorType>(CI.getType());
  Value *IntInput = CI.getOperand(0);

  SmallVector<Value*, 8> Elements(DestVecTy->getNumElements());
  if (!collectInsertionElements(IntInput, 0, Elements,
                                DestVecTy->getElementType(),
                                IC.getDataLayout().isBigEndian()))
    return nullptr;

  // If we succeeded, we know that all of the element are specified by Elements
  // or are zero if Elements has a null entry.  Recast this as a set of
  // insertions.
  Value *Result = Constant::getNullValue(CI.getType());
  for (unsigned i = 0, e = Elements.size(); i != e; ++i) {
    if (!Elements[i]) continue;  // Unset element.

    Result = IC.Builder->CreateInsertElement(Result, Elements[i],
                                             IC.Builder->getInt32(i));
  }

  return Result;
}

/// Canonicalize scalar bitcasts of extracted elements into a bitcast of the
/// vector followed by extract element. The backend tends to handle bitcasts of
/// vectors better than bitcasts of scalars because vector registers are
/// usually not type-specific like scalar integer or scalar floating-point.
static Instruction *canonicalizeBitCastExtElt(BitCastInst &BitCast,
                                              InstCombiner &IC,
                                              const DataLayout &DL) {
  // TODO: Create and use a pattern matcher for ExtractElementInst.
  auto *ExtElt = dyn_cast<ExtractElementInst>(BitCast.getOperand(0));
  if (!ExtElt || !ExtElt->hasOneUse())
    return nullptr;

  // The bitcast must be to a vectorizable type, otherwise we can't make a new
  // type to extract from.
  Type *DestType = BitCast.getType();
  if (!VectorType::isValidElementType(DestType))
    return nullptr;

  unsigned NumElts = ExtElt->getVectorOperandType()->getNumElements();
  auto *NewVecType = VectorType::get(DestType, NumElts);
  auto *NewBC = IC.Builder->CreateBitCast(ExtElt->getVectorOperand(),
                                          NewVecType, "bc");
  return ExtractElementInst::Create(NewBC, ExtElt->getIndexOperand());
}

/// Change the type of a bitwise logic operation if we can eliminate a bitcast.
static Instruction *foldBitCastBitwiseLogic(BitCastInst &BitCast,
                                            InstCombiner::BuilderTy &Builder) {
  Type *DestTy = BitCast.getType();
  BinaryOperator *BO = nullptr;
  if (!DestTy->getScalarType()->isIntegerTy() ||
      !match(BitCast.getOperand(0), m_OneUse(m_BinOp(BO))) ||
      !BO->isBitwiseLogicOp())
    return nullptr;

  // FIXME: This transform is restricted to vector types to avoid backend
  // problems caused by creating potentially illegal operations. If a fix-up is
  // added to handle that situation, we can remove this check.
  if (!DestTy->isVectorTy() || !BO->getType()->isVectorTy())
    return nullptr;

  Value *X = nullptr;
  if (match(BO->getOperand(0), m_OneUse(m_BitCast(m_Value(X)))) &&
      X->getType() == DestTy && !isa<Constant>(X)) {
    // bitcast(logic(bitcast(X), Y)) --> logic'(X, bitcast(Y))
    Value *CastedOp1 = Builder.CreateBitCast(BO->getOperand(1), DestTy);
    return BinaryOperator::Create(BO->getOpcode(), X, CastedOp1);
  }

  if (match(BO->getOperand(1), m_OneUse(m_BitCast(m_Value(X)))) &&
      X->getType() == DestTy && !isa<Constant>(X)) {
    // bitcast(logic(Y, bitcast(X))) --> logic'(bitcast(Y), X)
    Value *CastedOp0 = Builder.CreateBitCast(BO->getOperand(0), DestTy);
    return BinaryOperator::Create(BO->getOpcode(), CastedOp0, X);
  }

  return nullptr;
}

/// Change the type of a select if we can eliminate a bitcast.
static Instruction *foldBitCastSelect(BitCastInst &BitCast,
                                      InstCombiner::BuilderTy &Builder) {
  Value *Cond = nullptr, *TVal = nullptr, *FVal = nullptr;
  if (!match(BitCast.getOperand(0),
             m_OneUse(m_Select(m_Value(Cond), m_Value(TVal), m_Value(FVal)))))
    return nullptr;

  // A vector select must maintain the same number of elements in its operands.
  Type *CondTy = Cond->getType();
  Type *DestTy = BitCast.getType();
  if (CondTy->isVectorTy()) {
    if (!DestTy->isVectorTy())
      return nullptr;
    if (DestTy->getVectorNumElements() != CondTy->getVectorNumElements())
      return nullptr;
  }

  // FIXME: This transform is restricted from changing the select between
  // scalars and vectors to avoid backend problems caused by creating
  // potentially illegal operations. If a fix-up is added to handle that
  // situation, we can remove this check.
  if (DestTy->isVectorTy() != TVal->getType()->isVectorTy())
    return nullptr;

  auto *Sel = cast<Instruction>(BitCast.getOperand(0));
  Value *X = nullptr;
  if (match(TVal, m_OneUse(m_BitCast(m_Value(X)))) && X->getType() == DestTy &&
      !isa<Constant>(X)) {
    // bitcast(select(Cond, bitcast(X), Y)) --> select'(Cond, X, bitcast(Y))
    Value *CastedVal = Builder.CreateBitCast(FVal, DestTy);
    return SelectInst::Create(Cond, X, CastedVal, "", nullptr, Sel);
  }

  if (match(FVal, m_OneUse(m_BitCast(m_Value(X)))) && X->getType() == DestTy &&
      !isa<Constant>(X)) {
    // bitcast(select(Cond, Y, bitcast(X))) --> select'(Cond, bitcast(Y), X)
    Value *CastedVal = Builder.CreateBitCast(TVal, DestTy);
    return SelectInst::Create(Cond, CastedVal, X, "", nullptr, Sel);
  }

  return nullptr;
}

/// Check if all users of CI are StoreInsts.
static bool hasStoreUsersOnly(CastInst &CI) {
  for (User *U : CI.users()) {
    if (!isa<StoreInst>(U))
      return false;
  }
  return true;
}

/// This function handles following case
///
///     A  ->  B    cast
///     PHI
///     B  ->  A    cast
///
/// All the related PHI nodes can be replaced by new PHI nodes with type A.
/// The uses of \p CI can be changed to the new PHI node corresponding to \p PN.
Instruction *InstCombiner::optimizeBitCastFromPhi(CastInst &CI, PHINode *PN) {
//{{{ INTEL_LOCAL
  // Temporarily disable optimization of bitcasts through phi as it creates
  // significant amount of PHI nodes under certain patterns. As IGC has no
  // general register coalescing, these PHI nodes won't be coalesced. It
  // results in huge register pressure and slowdown lots of benchmarks
  // significantly.
  return nullptr;
//}}} INTEL_LOCAL
  // BitCast used by Store can be handled in InstCombineLoadStoreAlloca.cpp.
  if (hasStoreUsersOnly(CI))
    return nullptr;

  Value *Src = CI.getOperand(0);
  Type *SrcTy = Src->getType();         // Type B
  Type *DestTy = CI.getType();          // Type A

  SmallVector<PHINode *, 4> PhiWorklist;
  SmallSetVector<PHINode *, 4> OldPhiNodes;

  // Find all of the A->B casts and PHI nodes.
  // We need to inpect all related PHI nodes, but PHIs can be cyclic, so
  // OldPhiNodes is used to track all known PHI nodes, before adding a new
  // PHI to PhiWorklist, it is checked against and added to OldPhiNodes first.
  PhiWorklist.push_back(PN);
  OldPhiNodes.insert(PN);
  while (!PhiWorklist.empty()) {
    auto *OldPN = PhiWorklist.pop_back_val();
    for (Value *IncValue : OldPN->incoming_values()) {
      if (isa<Constant>(IncValue))
        continue;

      if (auto *LI = dyn_cast<LoadInst>(IncValue)) {
        // If there is a sequence of one or more load instructions, each loaded
        // value is used as address of later load instruction, bitcast is
        // necessary to change the value type, don't optimize it. For
        // simplicity we give up if the load address comes from another load.
        Value *Addr = LI->getOperand(0);
        if (Addr == &CI || isa<LoadInst>(Addr))
          return nullptr;
        if (LI->hasOneUse() && LI->isSimple())
          continue;
        // If a LoadInst has more than one use, changing the type of loaded
        // value may create another bitcast.
        return nullptr;
      }

      if (auto *PNode = dyn_cast<PHINode>(IncValue)) {
        if (OldPhiNodes.insert(PNode))
          PhiWorklist.push_back(PNode);
        continue;
      }

      auto *BCI = dyn_cast<BitCastInst>(IncValue);
      // We can't handle other instructions.
      if (!BCI)
        return nullptr;

      // Verify it's a A->B cast.
      Type *TyA = BCI->getOperand(0)->getType();
      Type *TyB = BCI->getType();
      if (TyA != DestTy || TyB != SrcTy)
        return nullptr;
    }
  }

  // For each old PHI node, create a corresponding new PHI node with a type A.
  SmallDenseMap<PHINode *, PHINode *> NewPNodes;
  for (auto *OldPN : OldPhiNodes) {
    Builder->SetInsertPoint(OldPN);
    PHINode *NewPN = Builder->CreatePHI(DestTy, OldPN->getNumOperands());
    errs() << "NewPN: " << *NewPN << '\n';
    NewPNodes[OldPN] = NewPN;
  }

  // Fill in the operands of new PHI nodes.
  for (auto *OldPN : OldPhiNodes) {
    PHINode *NewPN = NewPNodes[OldPN];
    for (unsigned j = 0, e = OldPN->getNumOperands(); j != e; ++j) {
      Value *V = OldPN->getOperand(j);
      Value *NewV = nullptr;
      if (auto *C = dyn_cast<Constant>(V)) {
        NewV = ConstantExpr::getBitCast(C, DestTy);
      } else if (auto *LI = dyn_cast<LoadInst>(V)) {
        Builder->SetInsertPoint(LI->getNextNode());
        NewV = Builder->CreateBitCast(LI, DestTy);
        Worklist.Add(LI);
      } else if (auto *BCI = dyn_cast<BitCastInst>(V)) {
        NewV = BCI->getOperand(0);
      } else if (auto *PrevPN = dyn_cast<PHINode>(V)) {
        NewV = NewPNodes[PrevPN];
      }
      assert(NewV);
      NewPN->addIncoming(NewV, OldPN->getIncomingBlock(j));
    }
  }

  // If there is a store with type B, change it to type A.
  for (User *U : PN->users()) {
    auto *SI = dyn_cast<StoreInst>(U);
    if (SI && SI->isSimple() && SI->getOperand(0) == PN) {
      Builder->SetInsertPoint(SI);
      auto *NewBC =
          cast<BitCastInst>(Builder->CreateBitCast(NewPNodes[PN], SrcTy));
      SI->setOperand(0, NewBC);
      Worklist.Add(SI);
      assert(hasStoreUsersOnly(*NewBC));
    }
  }

  return replaceInstUsesWith(CI, NewPNodes[PN]);
}

Instruction *InstCombiner::visitBitCast(BitCastInst &CI) {
  // If the operands are integer typed then apply the integer transforms,
  // otherwise just apply the common ones.
  Value *Src = CI.getOperand(0);
  Type *SrcTy = Src->getType();
  Type *DestTy = CI.getType();

  // Get rid of casts from one type to the same type. These are useless and can
  // be replaced by the operand.
  if (DestTy == Src->getType())
    return replaceInstUsesWith(CI, Src);

  if (PointerType *DstPTy = dyn_cast<PointerType>(DestTy)) {
    PointerType *SrcPTy = cast<PointerType>(SrcTy);
    Type *DstElTy = DstPTy->getElementType();
    Type *SrcElTy = SrcPTy->getElementType();

    // If we are casting a alloca to a pointer to a type of the same
    // size, rewrite the allocation instruction to allocate the "right" type.
    // There is no need to modify malloc calls because it is their bitcast that
    // needs to be cleaned up.
    if (AllocaInst *AI = dyn_cast<AllocaInst>(Src))
      if (Instruction *V = PromoteCastOfAllocation(CI, *AI))
        return V;

    // When the type pointed to is not sized the cast cannot be
    // turned into a gep.
    Type *PointeeType =
        cast<PointerType>(Src->getType()->getScalarType())->getElementType();
    if (!PointeeType->isSized())
      return nullptr;

    // If the source and destination are pointers, and this cast is equivalent
    // to a getelementptr X, 0, 0, 0...  turn it into the appropriate gep.
    // This can enhance SROA and other transforms that want type-safe pointers.
    unsigned NumZeros = 0;
    while (SrcElTy != DstElTy &&
           isa<CompositeType>(SrcElTy) && !SrcElTy->isPointerTy() &&
           SrcElTy->getNumContainedTypes() /* not "{}" */) {
      SrcElTy = cast<CompositeType>(SrcElTy)->getTypeAtIndex(0U);
      ++NumZeros;
    }

    // If we found a path from the src to dest, create the getelementptr now.
    if (SrcElTy == DstElTy) {
      SmallVector<Value *, 8> Idxs(NumZeros + 1, Builder->getInt32(0));
      return GetElementPtrInst::CreateInBounds(Src, Idxs);
    }
  }

  if (VectorType *DestVTy = dyn_cast<VectorType>(DestTy)) {
    if (DestVTy->getNumElements() == 1 && !SrcTy->isVectorTy()) {
      Value *Elem = Builder->CreateBitCast(Src, DestVTy->getElementType());
      return InsertElementInst::Create(UndefValue::get(DestTy), Elem,
                     Constant::getNullValue(Type::getInt32Ty(CI.getContext())));
      // FIXME: Canonicalize bitcast(insertelement) -> insertelement(bitcast)
    }

    if (isa<IntegerType>(SrcTy)) {
      // If this is a cast from an integer to vector, check to see if the input
      // is a trunc or zext of a bitcast from vector.  If so, we can replace all
      // the casts with a shuffle and (potentially) a bitcast.
      if (isa<TruncInst>(Src) || isa<ZExtInst>(Src)) {
        CastInst *SrcCast = cast<CastInst>(Src);
        if (BitCastInst *BCIn = dyn_cast<BitCastInst>(SrcCast->getOperand(0)))
          if (isa<VectorType>(BCIn->getOperand(0)->getType()))
            if (Instruction *I = optimizeVectorResize(BCIn->getOperand(0),
                                               cast<VectorType>(DestTy), *this))
              return I;
      }

      // If the input is an 'or' instruction, we may be doing shifts and ors to
      // assemble the elements of the vector manually.  Try to rip the code out
      // and replace it with insertelements.
      if (Value *V = optimizeIntegerToVectorInsertions(CI, *this))
        return replaceInstUsesWith(CI, V);
    }
  }

  if (VectorType *SrcVTy = dyn_cast<VectorType>(SrcTy)) {
    if (SrcVTy->getNumElements() == 1) {
      // If our destination is not a vector, then make this a straight
      // scalar-scalar cast.
      if (!DestTy->isVectorTy()) {
        Value *Elem =
          Builder->CreateExtractElement(Src,
                     Constant::getNullValue(Type::getInt32Ty(CI.getContext())));
        return CastInst::Create(Instruction::BitCast, Elem, DestTy);
      }

      // Otherwise, see if our source is an insert. If so, then use the scalar
      // component directly.
      if (InsertElementInst *IEI =
            dyn_cast<InsertElementInst>(CI.getOperand(0)))
        return CastInst::Create(Instruction::BitCast, IEI->getOperand(1),
                                DestTy);
    }
  }

  if (ShuffleVectorInst *SVI = dyn_cast<ShuffleVectorInst>(Src)) {
    // Okay, we have (bitcast (shuffle ..)).  Check to see if this is
    // a bitcast to a vector with the same # elts.
    if (SVI->hasOneUse() && DestTy->isVectorTy() &&
        DestTy->getVectorNumElements() == SVI->getType()->getNumElements() &&
        SVI->getType()->getNumElements() ==
        SVI->getOperand(0)->getType()->getVectorNumElements()) {
      BitCastInst *Tmp;
      // If either of the operands is a cast from CI.getType(), then
      // evaluating the shuffle in the casted destination's type will allow
      // us to eliminate at least one cast.
      if (((Tmp = dyn_cast<BitCastInst>(SVI->getOperand(0))) &&
           Tmp->getOperand(0)->getType() == DestTy) ||
          ((Tmp = dyn_cast<BitCastInst>(SVI->getOperand(1))) &&
           Tmp->getOperand(0)->getType() == DestTy)) {
        Value *LHS = Builder->CreateBitCast(SVI->getOperand(0), DestTy);
        Value *RHS = Builder->CreateBitCast(SVI->getOperand(1), DestTy);
        // Return a new shuffle vector.  Use the same element ID's, as we
        // know the vector types match #elts.
        return new ShuffleVectorInst(LHS, RHS, SVI->getOperand(2));
      }
    }
  }

  // Handle the A->B->A cast, and there is an intervening PHI node.
  if (PHINode *PN = dyn_cast<PHINode>(Src))
    if (Instruction *I = optimizeBitCastFromPhi(CI, PN))
      return I;

  if (Instruction *I = canonicalizeBitCastExtElt(CI, *this, DL))
    return I;

  if (Instruction *I = foldBitCastBitwiseLogic(CI, *Builder))
    return I;

  if (Instruction *I = foldBitCastSelect(CI, *Builder))
    return I;

  if (SrcTy->isPointerTy())
    return commonPointerCastTransforms(CI);
  return commonCastTransforms(CI);
}

Instruction *InstCombiner::visitAddrSpaceCast(AddrSpaceCastInst &CI) {
  // If the destination pointer element type is not the same as the source's
  // first do a bitcast to the destination type, and then the addrspacecast.
  // This allows the cast to be exposed to other transforms.
  Value *Src = CI.getOperand(0);
  PointerType *SrcTy = cast<PointerType>(Src->getType()->getScalarType());
  PointerType *DestTy = cast<PointerType>(CI.getType()->getScalarType());

  Type *DestElemTy = DestTy->getElementType();
  if (SrcTy->getElementType() != DestElemTy) {
    Type *MidTy = PointerType::get(DestElemTy, SrcTy->getAddressSpace());
    if (VectorType *VT = dyn_cast<VectorType>(CI.getType())) {
      // Handle vectors of pointers.
      MidTy = VectorType::get(MidTy, VT->getNumElements());
    }

    Value *NewBitCast = Builder->CreateBitCast(Src, MidTy);
    return new AddrSpaceCastInst(NewBitCast, CI.getType());
  }

  return commonPointerCastTransforms(CI);
}
#include "common/LLVMWarningsPop.hpp"
