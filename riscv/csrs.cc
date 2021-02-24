// See LICENSE for license details.

#include "csrs.h"
// For processor_t:
#include "processor.h"
#include "mmu.h"
// For get_field():
#include "decode.h"
// For trap_virtual_instruction and trap_illegal_instruction:
#include "trap.h"


// implement class csr_t
csr_t::csr_t(processor_t* const proc, const reg_t addr):
  proc(proc),
  address(addr),
  csr_priv(get_field(addr, 0x300)),
  csr_read_only(get_field(addr, 0xC00) == 3) {
}

void csr_t::verify_permissions(insn_t insn, bool write) const {
  // Check permissions. Raise virtual-instruction exception if V=1,
  // privileges are insufficient, and the CSR belongs to supervisor or
  // hypervisor. Raise illegal-instruction exception otherwise.
  state_t* const state = proc->get_state();
  unsigned priv = state->prv == PRV_S && !state->v ? PRV_HS : state->prv;

  if ((csr_priv == PRV_S && !proc->supports_extension('S')) ||
      (csr_priv == PRV_HS && !proc->supports_extension('H')))
    throw trap_illegal_instruction(insn.bits());

  if ((write && csr_read_only) || priv < csr_priv) {
    if (state->v && csr_priv <= PRV_HS)
      throw trap_virtual_instruction(insn.bits());
    throw trap_illegal_instruction(insn.bits());
  }
}


csr_t::~csr_t() {
}

// implement class logged_csr_t
logged_csr_t::logged_csr_t(processor_t* const proc, const reg_t addr):
  csr_t(proc, addr) {
}

void logged_csr_t::write(const reg_t val) noexcept {
  if (unlogged_write(val)) {
#if defined(RISCV_ENABLE_COMMITLOG)
    proc->get_state()->log_reg_write[((address) << 4) | 4] = {read(), 0};
#endif
  }
}

// implement class basic_csr_t
basic_csr_t::basic_csr_t(processor_t* const proc, const reg_t addr, const reg_t init):
  logged_csr_t(proc, addr),
  val(init) {
}

reg_t basic_csr_t::read() const noexcept {
  return val;
}

bool basic_csr_t::unlogged_write(const reg_t val) noexcept {
  this->val = val;
  return true;
}


// implement class pmpaddr_csr_t
pmpaddr_csr_t::pmpaddr_csr_t(processor_t* const proc, const reg_t addr):
  logged_csr_t(proc, addr),
  val(0),
  pmpidx(address - CSR_PMPADDR0) {
}


void pmpaddr_csr_t::verify_permissions(insn_t insn, bool write) const {
  logged_csr_t::verify_permissions(insn, write);
  // If n_pmp is zero, that means pmp is not implemented hence raise
  // trap if it tries to access the csr. I would prefer to implement
  // this by not instantiating any pmpaddr_csr_t for these regs, but
  // n_pmp can change after reset() is run.
  if (proc->n_pmp == 0)
    throw trap_illegal_instruction(insn.bits());
}


reg_t pmpaddr_csr_t::read() const noexcept {
  state_t* const state = proc->get_state();
  if ((state->pmpcfg[pmpidx] & PMP_A) >= PMP_NAPOT)
    return val | (~proc->pmp_tor_mask() >> 1);
  return val & proc->pmp_tor_mask();
}


bool pmpaddr_csr_t::unlogged_write(const reg_t val) noexcept {
  // If no PMPs are configured, disallow access to all. Otherwise,
  // allow access to all, but unimplemented ones are hardwired to
  // zero. Note that n_pmp can change after reset(); otherwise I would
  // implement this in state_t::reset() by instantiating the correct
  // number of pmpaddr_csr_t.
  if (proc->n_pmp == 0)
    return false;

  state_t* const state = proc->get_state();
  bool locked = state->pmpcfg[pmpidx] & PMP_L;
  if (pmpidx < proc->n_pmp && !locked && !next_locked_and_tor()) {
    this->val = val & ((reg_t(1) << (MAX_PADDR_BITS - PMP_SHIFT)) - 1);
  }
  else
    return false;
  proc->get_mmu()->flush_tlb();
  return true;
}

bool pmpaddr_csr_t::next_locked_and_tor() const noexcept {
  state_t* const state = proc->get_state();
  if (pmpidx >= state->max_pmp) return false;  // this is the last entry
  bool next_locked = state->pmpcfg[pmpidx+1] & PMP_L;
  bool next_tor = (state->pmpcfg[pmpidx+1] & PMP_A) == PMP_TOR;
  return next_locked && next_tor;
}


reg_t pmpaddr_csr_t::tor_paddr() const noexcept {
  return (val & proc->pmp_tor_mask()) << PMP_SHIFT;
}


reg_t pmpaddr_csr_t::tor_base_paddr() const noexcept {
  if (pmpidx == 0) return 0;  // entry 0 always uses 0 as base
  state_t* const state = proc->get_state();
  return state->pmpaddr[pmpidx-1]->tor_paddr();
}


reg_t pmpaddr_csr_t::napot_mask() const noexcept {
  state_t* const state = proc->get_state();
  bool is_na4 = (state->pmpcfg[pmpidx] & PMP_A) == PMP_NA4;
  reg_t mask = (val << 1) | (!is_na4) | ~proc->pmp_tor_mask();
  return ~(mask & ~(mask + 1)) << PMP_SHIFT;
}


bool pmpaddr_csr_t::match4(reg_t addr) const noexcept {
  state_t* const state = proc->get_state();
  uint8_t cfg = state->pmpcfg[pmpidx];
  if ((cfg & PMP_A) == 0) return false;
  bool is_tor = (cfg & PMP_A) == PMP_TOR;
  if (is_tor) return tor_base_paddr() <= addr && addr < tor_paddr();
  // NAPOT or NA4:
  return ((addr ^ tor_paddr()) & napot_mask()) == 0;
}


bool pmpaddr_csr_t::subset_match(reg_t addr, reg_t len) const noexcept {
  if ((addr | len) & (len - 1))
    abort();
  state_t* const state = proc->get_state();
  reg_t base = tor_base_paddr();
  reg_t tor = tor_paddr();
  uint8_t cfg = state->pmpcfg[pmpidx];

  if ((cfg & PMP_A) == 0) return false;

  bool is_tor = (cfg & PMP_A) == PMP_TOR;
  bool begins_after_lower = addr >= base;
  bool begins_after_upper = addr >= tor;
  bool ends_before_lower = (addr & -len) < (base & -len);
  bool ends_before_upper = (addr & -len) < (tor & -len);
  bool tor_homogeneous = ends_before_lower || begins_after_upper ||
    (begins_after_lower && ends_before_upper);

  bool mask_homogeneous = ~(napot_mask() << 1) & len;
  bool napot_homogeneous = mask_homogeneous || ((addr ^ tor) / len) != 0;

  return !(is_tor ? tor_homogeneous : napot_homogeneous);
}

// implement class pmpcfg_csr_t
pmpcfg_csr_t::pmpcfg_csr_t(processor_t* const proc, const reg_t addr):
  logged_csr_t(proc, addr) {
}

reg_t pmpcfg_csr_t::read() const noexcept {
  state_t* const state = proc->get_state();
  reg_t cfg_res = 0;
  for (size_t i0 = (address - CSR_PMPCFG0) * 4, i = i0; i < i0 + proc->get_xlen() / 8 && i < state->max_pmp; i++)
    cfg_res |= reg_t(state->pmpcfg[i]) << (8 * (i - i0));
  return cfg_res;
}

bool pmpcfg_csr_t::unlogged_write(const reg_t val) noexcept {
  if (proc->n_pmp == 0)
    return false;

  state_t* const state = proc->get_state();
  bool write_success = false;
  for (size_t i0 = (address - CSR_PMPCFG0) * 4, i = i0; i < i0 + proc->get_xlen() / 8; i++) {
    if (i < proc->n_pmp) {
      if (!(state->pmpcfg[i] & PMP_L)) {
        uint8_t cfg = (val >> (8 * (i - i0))) & (PMP_R | PMP_W | PMP_X | PMP_A | PMP_L);
        cfg &= ~PMP_W | ((cfg & PMP_R) ? PMP_W : 0); // Disallow R=0 W=1
        if (proc->lg_pmp_granularity != PMP_SHIFT && (cfg & PMP_A) == PMP_NA4)
          cfg |= PMP_NAPOT; // Disallow A=NA4 when granularity > 4
        state->pmpcfg[i] = cfg;
      }
      write_success = true;
    }
  }
  proc->get_mmu()->flush_tlb();
  return write_success;
}
