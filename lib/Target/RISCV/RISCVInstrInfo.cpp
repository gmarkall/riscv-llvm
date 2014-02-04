//===-- RISCVInstrInfo.cpp - RISCV instruction information ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the RISCV implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "RISCVInstrInfo.h"
#include "RISCVInstrBuilder.h"
#include "RISCVTargetMachine.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

#define GET_INSTRINFO_CTOR
#define GET_INSTRMAP_INFO
#include "RISCVGenInstrInfo.inc"

using namespace llvm;

RISCVInstrInfo::RISCVInstrInfo(RISCVTargetMachine &tm)
  : RISCVGenInstrInfo(RISCV::ADJCALLSTACKDOWN, RISCV::ADJCALLSTACKUP),
    RI(tm, *this), TM(tm) {
}

// If MI is a simple load or store for a frame object, return the register
// it loads or stores and set FrameIndex to the index of the frame object.
// Return 0 otherwise.
//
// Flag is SimpleLoad for loads and SimpleStore for stores.
static int isSimpleMove(const MachineInstr *MI, int &FrameIndex, int Flag) {
  const MCInstrDesc &MCID = MI->getDesc();
  if ((MCID.TSFlags & Flag) &&
      MI->getOperand(1).isFI() &&
      MI->getOperand(2).getImm() == 0 &&
      MI->getOperand(3).getReg() == 0) {
    FrameIndex = MI->getOperand(1).getIndex();
    return MI->getOperand(0).getReg();
  }
  return 0;
}

unsigned RISCVInstrInfo::isLoadFromStackSlot(const MachineInstr *MI,
                                               int &FrameIndex) const {
  return isSimpleMove(MI, FrameIndex, RISCVII::SimpleLoad);
}

unsigned RISCVInstrInfo::isStoreToStackSlot(const MachineInstr *MI,
                                              int &FrameIndex) const {
  return isSimpleMove(MI, FrameIndex, RISCVII::SimpleStore);
}

/// Adjust SP by Amount bytes.
void RISCVInstrInfo::adjustStackPtr(unsigned SP, int64_t Amount,
                                     MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator I) const {
  const RISCVSubtarget &STI = TM.getSubtarget<RISCVSubtarget>();
  DebugLoc DL = I != MBB.end() ? I->getDebugLoc() : DebugLoc();
  unsigned ADD =  STI.isRV64() ? RISCV::ADD64 : RISCV::ADD;
  unsigned ADDI = STI.isRV64() ? RISCV::ADDI64 : RISCV::ADDI;

  if (isInt<12>(Amount))// addi sp, sp, amount
    BuildMI(MBB, I, DL, get(ADDI), SP).addReg(SP).addImm(Amount);
  else { // Expand immediate that doesn't fit in 12-bit.
    unsigned Reg;
    loadImmediate(MBB, I, &Reg, Amount);
    BuildMI(MBB, I, DL, get(ADD), SP).addReg(SP).addReg(Reg, RegState::Kill);
  }
}

bool RISCVInstrInfo::AnalyzeBranch(MachineBasicBlock &MBB,
                                     MachineBasicBlock *&TBB,
                                     MachineBasicBlock *&FBB,
                                     SmallVectorImpl<MachineOperand> &Cond,
                                     bool AllowModify) const {
  // Most of the code and comments here are boilerplate.

  // Start from the bottom of the block and work up, examining the
  // terminator instructions.
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugValue())
      continue;

    // Working from the bottom, when we see a non-terminator instruction, we're
    // done.
    if (!isUnpredicatedTerminator(I))
      break;

    // A terminator that isn't a branch can't easily be handled by this
    // analysis.
    unsigned ThisCond;
    const MachineOperand *ThisTarget;
    if (!isBranch(I, ThisCond, ThisTarget))
      return true;

    // Can't handle indirect branches.
    if (!ThisTarget->isMBB())
      return true;

    if (ThisCond == RISCV::CCMASK_ANY) {
      // Handle unconditional branches.
      if (!AllowModify) {
        TBB = ThisTarget->getMBB();
        continue;
      }

      // If the block has any instructions after a JMP, delete them.
      while (llvm::next(I) != MBB.end())
        llvm::next(I)->eraseFromParent();

      Cond.clear();
      FBB = 0;

      // Delete the JMP if it's equivalent to a fall-through.
/*For now don't do this as a test
      if (MBB.isLayoutSuccessor(ThisTarget->getMBB())) {
        TBB = 0;
        I->eraseFromParent();
        I = MBB.end();
        continue;
      }
*/

      // TBB is used to indicate the unconditinal destination.
      TBB = ThisTarget->getMBB();
      continue;
    }

    // Working from the bottom, handle the first conditional branch.
    if (Cond.empty()) {
      // FIXME: add X86-style branch swap
      FBB = TBB;
      TBB = ThisTarget->getMBB();
      Cond.push_back(MachineOperand::CreateImm(ThisCond));
      //push remaining operands
      for (int i=0; i<(I->getNumExplicitOperands()); i++)
        Cond.push_back(I->getOperand(i));

      continue;
    }

    // Handle subsequent conditional branches.
    assert(Cond.size() <= 4);
    assert(TBB);

    // Only handle the case where all conditional branches branch to the same
    // destination.
    if (TBB != ThisTarget->getMBB())
      return true;

    // If the conditions are the same, we can leave them alone.
    unsigned OldCond = Cond[0].getImm();
    if (OldCond == ThisCond)
      continue;

    // FIXME: Try combining conditions like X86 does.
  }

  return false;
}

unsigned RISCVInstrInfo::RemoveBranch(MachineBasicBlock &MBB) const {
  // Most of the code and comments here are boilerplate.
  MachineBasicBlock::iterator I = MBB.end();
  unsigned Count = 0;

  while (I != MBB.begin()) {
    --I;
    if (I->isDebugValue())
      continue;
    unsigned Cond;
    const MachineOperand *Target;
    if (!isBranch(I, Cond, Target))
      break;
    if (!Target->isMBB())
      break;
    // Remove the branch.
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }

  return Count;
}

unsigned
RISCVInstrInfo::InsertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                               MachineBasicBlock *FBB,
                               const SmallVectorImpl<MachineOperand> &Cond,
                               DebugLoc DL) const {
  // Shouldn't be a fall through.
  assert(TBB && "InsertBranch must not be told to insert a fallthrough");
  assert(Cond.size() <= 4 &&
         "RISCV branch conditions have less than four components!");

  if (Cond.empty()) {
    // Unconditional branch
    assert(!FBB && "Unconditional branch with multiple successors!");
    BuildMI(&MBB, DL, get(RISCV::J)).addMBB(TBB);
    return 1;
  }

  // Conditional branch.
  unsigned Count = 0;
  unsigned CC = Cond[0].getImm();
  switch(CC) {
    case RISCV::CCMASK_CMP_EQ:
      BuildMI(&MBB, DL, get(RISCV::BEQ)).addMBB(TBB).addReg(Cond[1].getReg())
          .addReg(Cond[2].getReg());
      break;
    case RISCV::CCMASK_CMP_NE:
      BuildMI(&MBB, DL, get(RISCV::BNE)).addMBB(TBB).addReg(Cond[1].getReg())
          .addReg(Cond[2].getReg());
      break;
    case RISCV::CCMASK_CMP_LT:
      BuildMI(&MBB, DL, get(RISCV::BLT)).addMBB(TBB).addReg(Cond[1].getReg())
          .addReg(Cond[2].getReg());
      break;
    case (RISCV::CCMASK_CMP_LT | RISCV::CCMASK_CMP_UO):
      BuildMI(&MBB, DL, get(RISCV::BLTU)).addMBB(TBB).addReg(Cond[1].getReg())
          .addReg(Cond[2].getReg());
      break;
    case RISCV::CCMASK_CMP_GE:
      BuildMI(&MBB, DL, get(RISCV::BGE)).addMBB(TBB).addReg(Cond[1].getReg())
          .addReg(Cond[2].getReg());
      break;
    case (RISCV::CCMASK_CMP_GE | RISCV::CCMASK_CMP_UO):
      BuildMI(&MBB, DL, get(RISCV::BGEU)).addMBB(TBB).addReg(Cond[1].getReg())
          .addReg(Cond[2].getReg());
      break;
    default:
      llvm_unreachable("Invalid branch condition code!");
  }
  ++Count;

  //TODO: does RISCV have any two way branches (assume not)
  if (FBB) {
    ++Count;
    llvm_unreachable("Can not insert two-way branch!");
  }
  return Count;
}

void
RISCVInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
			      MachineBasicBlock::iterator MBBI, DebugLoc DL,
			      unsigned DestReg, unsigned SrcReg,
			      bool KillSrc) const {

  unsigned Opcode;
  //TODO: when we are copying a phys reg do we want the value or bits for fp?
  if (RISCV::GR32BitRegClass.contains(DestReg, SrcReg))
    Opcode = RISCV::ORI;
  else if (RISCV::GR64BitRegClass.contains(DestReg, SrcReg))
    Opcode = RISCV::ORI64;
  else if (RISCV::FP32BitRegClass.contains(DestReg, SrcReg)){
    Opcode = RISCV::FSGNJ_S;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc))
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if (RISCV::FP64BitRegClass.contains(DestReg, SrcReg)) {
    Opcode = RISCV::FSGNJ_D;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc))
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if(RISCV::FP32BitRegClass.contains(SrcReg) &&
           RISCV::GR32BitRegClass.contains(DestReg)){
    Opcode = RISCV::FCVT_W_S_RDY;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if(RISCV::FP64BitRegClass.contains(SrcReg) &&
           RISCV::GR32BitRegClass.contains(DestReg)){
    Opcode = RISCV::FCVT_W_D_RDY;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if(RISCV::FP32BitRegClass.contains(SrcReg) &&
           RISCV::GR64BitRegClass.contains(DestReg)){
    Opcode = RISCV::FCVT_L_S_RDY;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if(RISCV::FP64BitRegClass.contains(SrcReg) &&
           RISCV::GR64BitRegClass.contains(DestReg)){
    Opcode = RISCV::FCVT_L_D_RDY;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if(RISCV::FP32BitRegClass.contains(DestReg) &&
           RISCV::GR32BitRegClass.contains(SrcReg)){
    Opcode = RISCV::FCVT_S_W_RDY;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if(RISCV::FP64BitRegClass.contains(DestReg) &&
           RISCV::GR32BitRegClass.contains(SrcReg)){
    Opcode = RISCV::FCVT_D_W_RDY;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if(RISCV::FP32BitRegClass.contains(DestReg) &&
           RISCV::GR64BitRegClass.contains(SrcReg)){
    Opcode = RISCV::FCVT_S_L_RDY;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else if(RISCV::FP64BitRegClass.contains(DestReg) &&
           RISCV::GR64BitRegClass.contains(SrcReg)){
    Opcode = RISCV::FCVT_D_L_RDY;
    BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }else
    llvm_unreachable("Impossible reg-to-reg copy");

  BuildMI(MBB, MBBI, DL, get(Opcode), DestReg)
    .addReg(SrcReg, getKillRegState(KillSrc))
    .addImm(0);
}

void
RISCVInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
				      MachineBasicBlock::iterator MBBI,
				      unsigned SrcReg, bool isKill,
				      int FrameIdx,
				      const TargetRegisterClass *RC,
				      const TargetRegisterInfo *TRI) const {
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();

  // Callers may expect a single instruction, so keep 128-bit moves
  // together for now and lower them after register allocation.
  unsigned LoadOpcode, StoreOpcode;
  getLoadStoreOpcodes(RC, LoadOpcode, StoreOpcode);
  addFrameReference(BuildMI(MBB, MBBI, DL, get(StoreOpcode))
		    .addReg(SrcReg, getKillRegState(isKill)), FrameIdx);
}

void
RISCVInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
				       MachineBasicBlock::iterator MBBI,
				       unsigned DestReg, int FrameIdx,
				       const TargetRegisterClass *RC,
				       const TargetRegisterInfo *TRI) const {
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();

  // Callers may expect a single instruction, so keep 128-bit moves
  // together for now and lower them after register allocation.
  unsigned LoadOpcode, StoreOpcode;
  getLoadStoreOpcodes(RC, LoadOpcode, StoreOpcode);
  addFrameReference(BuildMI(MBB, MBBI, DL, get(LoadOpcode), DestReg),
                    FrameIdx);
}

bool
RISCVInstrInfo::expandPostRAPseudo(MachineBasicBlock::iterator MI) const {
  switch (MI->getOpcode()) {

  default:
    return false;
  }
}

bool RISCVInstrInfo::
ReverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() <= 4 && "Invalid branch condition!");
  //Only need to switch the condition code, not the registers
  switch (Cond[0].getImm()) {
  case RISCV::CCMASK_CMP_EQ:
    Cond[0].setImm(RISCV::CCMASK_CMP_NE);
    return false;
  case RISCV::CCMASK_CMP_NE:
    Cond[0].setImm(RISCV::CCMASK_CMP_EQ);
    return false;
  case RISCV::CCMASK_CMP_LT:
    Cond[0].setImm(RISCV::CCMASK_CMP_GE);
    return false;
  case RISCV::CCMASK_CMP_GE:
    Cond[0].setImm(RISCV::CCMASK_CMP_LT);
    return false;
  case RISCV::CCMASK_CMP_LT | RISCV::CCMASK_CMP_UO:
    Cond[0].setImm(RISCV::CCMASK_CMP_GE | RISCV::CCMASK_CMP_UO);
    return false;
  case RISCV::CCMASK_CMP_GE | RISCV::CCMASK_CMP_UO:
    Cond[0].setImm(RISCV::CCMASK_CMP_LT | RISCV::CCMASK_CMP_UO);
    return false;
  //synth
  case RISCV::CCMASK_CMP_GT:
    Cond[0].setImm(RISCV::CCMASK_CMP_LE);
    return false;
  case RISCV::CCMASK_CMP_LE:
    Cond[0].setImm(RISCV::CCMASK_CMP_GT);
    return false;
  case RISCV::CCMASK_CMP_GT | RISCV::CCMASK_CMP_UO:
    Cond[0].setImm(RISCV::CCMASK_CMP_LE | RISCV::CCMASK_CMP_UO);
    return false;
  case RISCV::CCMASK_CMP_LE | RISCV::CCMASK_CMP_UO:
    Cond[0].setImm(RISCV::CCMASK_CMP_GT | RISCV::CCMASK_CMP_UO);
    return false;
  default:
    llvm_unreachable("Invalid branch condition!");
  }
}

bool RISCVInstrInfo::isBranch(const MachineInstr *MI, unsigned &Cond,
                                const MachineOperand *&Target) const {
  switch (MI->getOpcode()) {
  case RISCV::J:
  case RISCV::J64:
  case RISCV::JAL:
  case RISCV::JAL64:
  case RISCV::JALR:
  case RISCV::JALR64:
    Cond = RISCV::CCMASK_ANY;
    Target = &MI->getOperand(0);
    return true;
  case RISCV::BEQ:
  case RISCV::BEQ64:
  //case RISCV::BEQ64_32:
    Cond = RISCV::CCMASK_CMP_EQ;
    Target = &MI->getOperand(2);
    return true;
  case RISCV::BNE:
  case RISCV::BNE64:
    Cond = RISCV::CCMASK_CMP_NE;
    Target = &MI->getOperand(2);
    return true;
  case RISCV::BLT:
  case RISCV::BLT64:
    Cond = RISCV::CCMASK_CMP_LT;
    Target = &MI->getOperand(2);
    return true;
  case RISCV::BLTU:
  case RISCV::BLTU64:
    Cond = RISCV::CCMASK_CMP_LT | RISCV::CCMASK_CMP_UO;
    Target = &MI->getOperand(2);
    return true;
  case RISCV::BGE:
  case RISCV::BGE64:
    Cond = RISCV::CCMASK_CMP_GE;
    Target = &MI->getOperand(2);
    return true;
  case RISCV::BGEU:
  case RISCV::BGEU64:
    Cond = RISCV::CCMASK_CMP_GE | RISCV::CCMASK_CMP_UO;
    Target = &MI->getOperand(2);
    return true;
//synth
  case RISCV::BGT:
  case RISCV::BGT64:
    Cond = RISCV::CCMASK_CMP_GT;
    Target = &MI->getOperand(2);
    return true;
  case RISCV::BGTU:
  case RISCV::BGTU64:
    Cond = RISCV::CCMASK_CMP_GT | RISCV::CCMASK_CMP_UO;
    Target = &MI->getOperand(2);
    return true;
  case RISCV::BLE:
  case RISCV::BLE64:
    Cond = RISCV::CCMASK_CMP_LE;
    Target = &MI->getOperand(2);
    return true;
  case RISCV::BLEU:
  case RISCV::BLEU64:
    Cond = RISCV::CCMASK_CMP_LE | RISCV::CCMASK_CMP_UO;
    Target = &MI->getOperand(2);
    return true;
 

  default:
    assert(!MI->getDesc().isBranch() && "Unknown branch opcode");
    return false;
  }
}

void RISCVInstrInfo::getLoadStoreOpcodes(const TargetRegisterClass *RC,
                                           unsigned &LoadOpcode,
                                           unsigned &StoreOpcode) const {
  const RISCVSubtarget &STI = TM.getSubtarget<RISCVSubtarget>();
  if (RC == &RISCV::GR32BitRegClass ){//|| RC == &RISCV::ADDR32BitRegClass) {
    LoadOpcode = STI.isRV64() ? RISCV::LW64 : RISCV::LW;
    StoreOpcode = STI.isRV64() ? RISCV::SW64 : RISCV::SW;
  } else if (RC == &RISCV::GR64BitRegClass) {
    LoadOpcode = RISCV::LD;
    StoreOpcode = RISCV::SD;
  } else if (RC == &RISCV::FP32BitRegClass) {
    LoadOpcode = STI.isRV64() ? RISCV::FLW64 : RISCV::FLW;
    StoreOpcode = STI.isRV64() ? RISCV::FSW64 : RISCV::FSW;
  } else if (RC == &RISCV::FP64BitRegClass) {
    LoadOpcode = STI.isRV64() ? RISCV::FLD64 : RISCV::FLD;
    StoreOpcode = STI.isRV64() ? RISCV::FSD64 : RISCV::FSD;
  } else
    llvm_unreachable("Unsupported regclass to load or store");
}

unsigned RISCVInstrInfo::getOpcodeForOffset(unsigned Opcode,
                                              int64_t Offset) const {
  const MCInstrDesc &MCID = get(Opcode);
  int64_t Offset2 = Offset;
  if (isInt<12>(Offset) && isInt<12>(Offset2)) {
    return Opcode;
  }
  if (isInt<20>(Offset) && isInt<20>(Offset2)) {
      return Opcode;
  }
  return 0;
}

void RISCVInstrInfo::loadImmediate(MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator MBBI,
                                     unsigned *Reg, int64_t Value) const {
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();
  unsigned Opcode;
  MachineRegisterInfo &RegInfo = MBB.getParent()->getRegInfo();
  const RISCVSubtarget &STI = TM.getSubtarget<RISCVSubtarget>();
  const TargetRegisterClass *RC = STI.isRV64() ?
    &RISCV::GR64BitRegClass : &RISCV::GR32BitRegClass;
  unsigned ZERO = STI.isRV64() ? RISCV::zero_64 : RISCV::zero;

  //create virtual reg to store immediate
  *Reg = RegInfo.createVirtualRegister(RC);
  if (isInt<12>(Value)){
    Opcode = STI.isRV64() ? RISCV::ADDI64 : RISCV::ADDI;
    BuildMI(MBB, MBBI, DL, get(Opcode), *Reg).addReg(ZERO).addImm(Value);
  } else {
  //use LI to let assembler load immediate best
  BuildMI(MBB, MBBI, DL, get(RISCV::LI), *Reg).addImm(Value);
  }
}