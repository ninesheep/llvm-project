//===-------------- RISCVSExtWRemoval.cpp - MI sext.w Removal -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// This pass removes unneeded sext.w instructions at the MI level.
//
//===---------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVMachineFunctionInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-sextw-removal"

STATISTIC(NumRemovedSExtW, "Number of removed sign-extensions");
STATISTIC(NumTransformedToWInstrs,
          "Number of instructions transformed to W-ops");

static cl::opt<bool> DisableSExtWRemoval("riscv-disable-sextw-removal",
                                         cl::desc("Disable removal of sext.w"),
                                         cl::init(false), cl::Hidden);
namespace {

class RISCVSExtWRemoval : public MachineFunctionPass {
public:
  static char ID;

  RISCVSExtWRemoval() : MachineFunctionPass(ID) {
    initializeRISCVSExtWRemovalPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return "RISCV sext.w Removal"; }
};

} // end anonymous namespace

char RISCVSExtWRemoval::ID = 0;
INITIALIZE_PASS(RISCVSExtWRemoval, DEBUG_TYPE, "RISCV sext.w Removal", false,
                false)

FunctionPass *llvm::createRISCVSExtWRemovalPass() {
  return new RISCVSExtWRemoval();
}

// returns true if all uses of OrigMI only depend on the lower word of its
// output, so we can transform OrigMI to the corresponding W-version.
// TODO: handle multiple interdependent transformations
static bool hasAllWUsers(const MachineInstr &OrigMI, MachineRegisterInfo &MRI) {

  SmallPtrSet<const MachineInstr *, 4> Visited;
  SmallVector<const MachineInstr *, 4> Worklist;

  Worklist.push_back(&OrigMI);

  while (!Worklist.empty()) {
    const MachineInstr *MI = Worklist.pop_back_val();

    if (!Visited.insert(MI).second)
      continue;

    // Only handle instructions with one def.
    if (MI->getNumExplicitDefs() != 1)
      return false;

    for (auto &UserOp : MRI.use_operands(MI->getOperand(0).getReg())) {
      const MachineInstr *UserMI = UserOp.getParent();
      unsigned OpIdx = UserMI->getOperandNo(&UserOp);

      switch (UserMI->getOpcode()) {
      default:
        return false;

      case RISCV::ADDIW:
      case RISCV::ADDW:
      case RISCV::DIVUW:
      case RISCV::DIVW:
      case RISCV::MULW:
      case RISCV::REMUW:
      case RISCV::REMW:
      case RISCV::SLLIW:
      case RISCV::SLLW:
      case RISCV::SRAIW:
      case RISCV::SRAW:
      case RISCV::SRLIW:
      case RISCV::SRLW:
      case RISCV::SUBW:
      case RISCV::ROLW:
      case RISCV::RORW:
      case RISCV::RORIW:
      case RISCV::CLZW:
      case RISCV::CTZW:
      case RISCV::CPOPW:
      case RISCV::SLLI_UW:
      case RISCV::FMV_H_X:
      case RISCV::FMV_W_X:
      case RISCV::FCVT_H_W:
      case RISCV::FCVT_H_WU:
      case RISCV::FCVT_S_W:
      case RISCV::FCVT_S_WU:
      case RISCV::FCVT_D_W:
      case RISCV::FCVT_D_WU:
      case RISCV::SEXT_B:
      case RISCV::SEXT_H:
      case RISCV::ZEXT_H_RV64:
      case RISCV::PACK:
      case RISCV::PACKH:
      case RISCV::PACKW:
        break;

      // these overwrite higher input bits, otherwise the lower word of output
      // depends only on the lower word of input. So check their uses read W.
      case RISCV::SLLI:
        if (UserMI->getOperand(2).getImm() >= 32)
          break;
        Worklist.push_back(UserMI);
        break;
      case RISCV::ANDI:
        if (isUInt<11>(UserMI->getOperand(2).getImm()))
          break;
        Worklist.push_back(UserMI);
        break;
      case RISCV::ORI:
        if (!isUInt<11>(UserMI->getOperand(2).getImm()))
          break;
        Worklist.push_back(UserMI);
        break;

      case RISCV::SLL:
      case RISCV::BSET:
      case RISCV::BCLR:
      case RISCV::BINV:
        // Operand 2 is the shift amount which uses 6 bits.
        if (OpIdx == 2)
          break;
        Worklist.push_back(UserMI);
        break;

      case RISCV::SRA:
      case RISCV::SRL:
      case RISCV::ROL:
      case RISCV::ROR:
        // Operand 2 is the shift amount which uses 6 bits.
        if (OpIdx == 2)
          break;
        return false;

      case RISCV::ADD_UW:
      case RISCV::SH1ADD_UW:
      case RISCV::SH2ADD_UW:
      case RISCV::SH3ADD_UW:
        // Operand 1 is implicitly zero extended.
        if (OpIdx == 1)
          break;
        Worklist.push_back(UserMI);
        break;

      case RISCV::BEXTI:
        if (UserMI->getOperand(2).getImm() >= 32)
          return false;
        break;

      case RISCV::SB:
      case RISCV::SH:
      case RISCV::SW:
        // The first argument is the value to store.
        if (OpIdx != 0)
          return false;
        break;

      // For these, lower word of output in these operations, depends only on
      // the lower word of input. So, we check all uses only read lower word.
      case RISCV::COPY:
      case RISCV::PHI:

      case RISCV::ADD:
      case RISCV::ADDI:
      case RISCV::AND:
      case RISCV::MUL:
      case RISCV::OR:
      case RISCV::SUB:
      case RISCV::XOR:
      case RISCV::XORI:

      case RISCV::ANDN:
      case RISCV::BREV8:
      case RISCV::CLMUL:
      case RISCV::ORC_B:
      case RISCV::ORN:
      case RISCV::SH1ADD:
      case RISCV::SH2ADD:
      case RISCV::SH3ADD:
      case RISCV::XNOR:
      case RISCV::BSETI:
      case RISCV::BCLRI:
      case RISCV::BINVI:
        Worklist.push_back(UserMI);
        break;

      case RISCV::PseudoCCMOVGPR:
        // Either operand 4 or operand 5 is returned by this instruction. If
        // only the lower word of the result is used, then only the lower word
        // of operand 4 and 5 is used.
        if (OpIdx != 4 && OpIdx != 5)
          return false;
        Worklist.push_back(UserMI);
        break;
      }
    }
  }

  return true;
}

// This function returns true if the machine instruction always outputs a value
// where bits 63:32 match bit 31.
// TODO: Allocate a bit in TSFlags for the W instructions?
// TODO: Add other W instructions.
static bool isSignExtendingOpW(MachineInstr &MI, MachineRegisterInfo &MRI) {
  switch (MI.getOpcode()) {
  case RISCV::LUI:
  case RISCV::LW:
  case RISCV::ADDW:
  case RISCV::ADDIW:
  case RISCV::SUBW:
  case RISCV::MULW:
  case RISCV::SLLW:
  case RISCV::SLLIW:
  case RISCV::SRAW:
  case RISCV::SRAIW:
  case RISCV::SRLW:
  case RISCV::SRLIW:
  case RISCV::DIVW:
  case RISCV::DIVUW:
  case RISCV::REMW:
  case RISCV::REMUW:
  case RISCV::ROLW:
  case RISCV::RORW:
  case RISCV::RORIW:
  case RISCV::CLZW:
  case RISCV::CTZW:
  case RISCV::CPOPW:
  case RISCV::PACKW:
  case RISCV::FCVT_W_H:
  case RISCV::FCVT_WU_H:
  case RISCV::FCVT_W_S:
  case RISCV::FCVT_WU_S:
  case RISCV::FCVT_W_D:
  case RISCV::FCVT_WU_D:
  case RISCV::FMV_X_W:
  // The following aren't W instructions, but are either sign extended from a
  // smaller size, always outputs a small integer, or put zeros in bits 63:31.
  case RISCV::LBU:
  case RISCV::LHU:
  case RISCV::LB:
  case RISCV::LH:
  case RISCV::SLT:
  case RISCV::SLTI:
  case RISCV::SLTU:
  case RISCV::SLTIU:
  case RISCV::FEQ_H:
  case RISCV::FEQ_S:
  case RISCV::FEQ_D:
  case RISCV::FLT_H:
  case RISCV::FLT_S:
  case RISCV::FLT_D:
  case RISCV::FLE_H:
  case RISCV::FLE_S:
  case RISCV::FLE_D:
  case RISCV::SEXT_B:
  case RISCV::SEXT_H:
  case RISCV::ZEXT_H_RV64:
  case RISCV::FMV_X_H:
  case RISCV::BEXT:
  case RISCV::BEXTI:
  case RISCV::CLZ:
  case RISCV::CPOP:
  case RISCV::CTZ:
  case RISCV::PACKH:
    return true;
  // shifting right sufficiently makes the value 32-bit sign-extended
  case RISCV::SRAI:
    return MI.getOperand(2).getImm() >= 32;
  case RISCV::SRLI:
    return MI.getOperand(2).getImm() > 32;
  // The LI pattern ADDI rd, X0, imm is sign extended.
  case RISCV::ADDI:
    return MI.getOperand(1).isReg() && MI.getOperand(1).getReg() == RISCV::X0;
  // An ANDI with an 11 bit immediate will zero bits 63:11.
  case RISCV::ANDI:
    return isUInt<11>(MI.getOperand(2).getImm());
  // An ORI with an >11 bit immediate (negative 12-bit) will set bits 63:11.
  case RISCV::ORI:
    return !isUInt<11>(MI.getOperand(2).getImm());
  // Copying from X0 produces zero.
  case RISCV::COPY:
    return MI.getOperand(1).getReg() == RISCV::X0;

  }

  return false;
}

static bool isSignExtendedW(Register SrcReg, MachineRegisterInfo &MRI,
                            SmallPtrSetImpl<MachineInstr *> &FixableDef) {

  SmallPtrSet<const MachineInstr *, 4> Visited;
  SmallVector<MachineInstr *, 4> Worklist;

  auto AddRegDefToWorkList = [&](Register SrcReg) {
    if (!SrcReg.isVirtual())
      return false;
    MachineInstr *SrcMI = MRI.getVRegDef(SrcReg);
    if (!SrcMI)
      return false;
    // Add SrcMI to the worklist.
    Worklist.push_back(SrcMI);
    return true;
  };

  if (!AddRegDefToWorkList(SrcReg))
    return false;

  while (!Worklist.empty()) {
    MachineInstr *MI = Worklist.pop_back_val();

    // If we already visited this instruction, we don't need to check it again.
    if (!Visited.insert(MI).second)
      continue;

    // If this is a sign extending operation we don't need to look any further.
    if (isSignExtendingOpW(*MI, MRI))
      continue;

    // Is this an instruction that propagates sign extend?
    switch (MI->getOpcode()) {
    default:
      // Unknown opcode, give up.
      return false;
    case RISCV::COPY: {
      const MachineFunction *MF = MI->getMF();
      const RISCVMachineFunctionInfo *RVFI =
          MF->getInfo<RISCVMachineFunctionInfo>();

      // If this is the entry block and the register is livein, see if we know
      // it is sign extended.
      if (MI->getParent() == &MF->front()) {
        Register VReg = MI->getOperand(0).getReg();
        if (MF->getRegInfo().isLiveIn(VReg) && RVFI->isSExt32Register(VReg))
          continue;
      }

      // TODO: Handle returns from calls?

      if (!AddRegDefToWorkList(MI->getOperand(1).getReg()))
        return false;

      break;
    }

    // For these, we just need to check if the 1st operand is sign extended.
    case RISCV::BCLRI:
    case RISCV::BINVI:
    case RISCV::BSETI:
      if (MI->getOperand(2).getImm() >= 31)
        return false;
      [[fallthrough]];
    case RISCV::REM:
    case RISCV::ANDI:
    case RISCV::ORI:
    case RISCV::XORI: {
      // |Remainder| is always <= |Dividend|. If D is 32-bit, then so is R.
      // DIV doesn't work because of the edge case 0xf..f 8000 0000 / (long)-1
      // Logical operations use a sign extended 12-bit immediate.
      if (!AddRegDefToWorkList(MI->getOperand(1).getReg()))
        return false;

      break;
    }
    case RISCV::REMU:
    case RISCV::AND:
    case RISCV::OR:
    case RISCV::XOR:
    case RISCV::ANDN:
    case RISCV::ORN:
    case RISCV::XNOR:
    case RISCV::MAX:
    case RISCV::MAXU:
    case RISCV::MIN:
    case RISCV::MINU:
    case RISCV::PseudoCCMOVGPR:
    case RISCV::PHI: {
      // If all incoming values are sign-extended, the output of AND, OR, XOR,
      // MIN, MAX, or PHI is also sign-extended.

      // The input registers for PHI are operand 1, 3, ...
      // The input registers for PseudoCCMOVGPR are 4 and 5.
      // The input registers for others are operand 1 and 2.
      unsigned B = 1, E = 3, D = 1;
      if (MI->getOpcode() == RISCV::PHI) {
        E = MI->getNumOperands();
        D = 2;
      } else if (MI->getOpcode() == RISCV::PseudoCCMOVGPR) {
        B = 4;
        E = 6;
      }

      for (unsigned I = B; I != E; I += D) {
        if (!MI->getOperand(I).isReg())
          return false;

        if (!AddRegDefToWorkList(MI->getOperand(I).getReg()))
          return false;
      }

      break;
    }

    // With these opcode, we can "fix" them with the W-version
    // if we know all users of the result only rely on bits 31:0
    case RISCV::SLLI:
      // SLLIW reads the lowest 5 bits, while SLLI reads lowest 6 bits
      if (MI->getOperand(2).getImm() >= 32)
        return false;
      [[fallthrough]];
    case RISCV::ADDI:
    case RISCV::ADD:
    case RISCV::LD:
    case RISCV::LWU:
    case RISCV::MUL:
    case RISCV::SUB:
      if (hasAllWUsers(*MI, MRI)) {
        FixableDef.insert(MI);
        break;
      }
      return false;
    }
  }

  // If we get here, then every node we visited produces a sign extended value
  // or propagated sign extended values. So the result must be sign extended.
  return true;
}

static unsigned getWOp(unsigned Opcode) {
  switch (Opcode) {
  case RISCV::ADDI:
    return RISCV::ADDIW;
  case RISCV::ADD:
    return RISCV::ADDW;
  case RISCV::LD:
  case RISCV::LWU:
    return RISCV::LW;
  case RISCV::MUL:
    return RISCV::MULW;
  case RISCV::SLLI:
    return RISCV::SLLIW;
  case RISCV::SUB:
    return RISCV::SUBW;
  default:
    llvm_unreachable("Unexpected opcode for replacement with W variant");
  }
}

bool RISCVSExtWRemoval::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()) || DisableSExtWRemoval)
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
  const RISCVInstrInfo &TII = *ST.getInstrInfo();

  if (!ST.is64Bit())
    return false;

  bool MadeChange = false;

  for (MachineBasicBlock &MBB : MF) {
    for (auto I = MBB.begin(), IE = MBB.end(); I != IE;) {
      MachineInstr *MI = &*I++;

      // We're looking for the sext.w pattern ADDIW rd, rs1, 0.
      if (!RISCV::isSEXT_W(*MI))
        continue;

      Register SrcReg = MI->getOperand(1).getReg();

      SmallPtrSet<MachineInstr *, 4> FixableDefs;

      // If all definitions reaching MI sign-extend their output,
      // then sext.w is redundant
      if (!isSignExtendedW(SrcReg, MRI, FixableDefs))
        continue;

      Register DstReg = MI->getOperand(0).getReg();
      if (!MRI.constrainRegClass(SrcReg, MRI.getRegClass(DstReg)))
        continue;

      // Convert Fixable instructions to their W versions.
      for (MachineInstr *Fixable : FixableDefs) {
        LLVM_DEBUG(dbgs() << "Replacing " << *Fixable);
        Fixable->setDesc(TII.get(getWOp(Fixable->getOpcode())));
        Fixable->clearFlag(MachineInstr::MIFlag::NoSWrap);
        Fixable->clearFlag(MachineInstr::MIFlag::NoUWrap);
        Fixable->clearFlag(MachineInstr::MIFlag::IsExact);
        LLVM_DEBUG(dbgs() << "     with " << *Fixable);
        ++NumTransformedToWInstrs;
      }

      LLVM_DEBUG(dbgs() << "Removing redundant sign-extension\n");
      MRI.replaceRegWith(DstReg, SrcReg);
      MRI.clearKillFlags(SrcReg);
      MI->eraseFromParent();
      ++NumRemovedSExtW;
      MadeChange = true;
    }
  }

  return MadeChange;
}
