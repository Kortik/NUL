/*
 * Common code for NOVA programs.
 *
 * Copyright (C) 2008, Bernhard Kauer <bk@vmmon.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of Vancouver.
 *
 * Vancouver.nova is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * Vancouver.nova is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */
#pragma once
#include "service/string.h"
#include "service/cpu.h"
#include "service/logging.h"
#include "sys/hip.h"
#include "sys/syscalls.h"
#include "region.h"
#include "baseprogram.h"
#include "config.h"

#include <nul/capalloc.h>

extern "C" void __attribute__((noreturn)) __attribute__((regparm(1))) idc_reply_and_wait_fast(unsigned long mtr);
extern char __image_start, __image_end;

// cap map - maintains the list of free capability regions
RegionList<512> _cap_region;
// Not multi-threaded safe - lock by your own or use separate cap allocator -> alloc_cap/dealloc_cap in nul/capalloc.h !
unsigned alloc_cap_region(unsigned count, unsigned align_order) { return _cap_region.alloc(count, align_order); }
void dealloc_cap_region(unsigned base, unsigned count) { return _cap_region.add(Region(base, count)); }

/**
 * Contains common code for nova programms.
 */
class NovaProgram : public BaseProgram, public CapAllocator<NovaProgram>
{
  enum {
    VIRT_START       = 0x1000,
    UTCB_PAD         = 0x1000,
  };

 protected:
  Hip *     _hip;
  unsigned  _cap_block;


  // memory map
  RegionList<512> _free_virt;
  RegionList<512> _free_phys;
  RegionList<512> _virt_phys;

  /**
   * Alloc a region of virtual memory to put an EC into
   */
  Utcb * alloc_utcb() { return reinterpret_cast<Utcb *>(_free_virt.alloc(2*UTCB_PAD + sizeof(Utcb), 12) + UTCB_PAD); }

  /**
   * Create an ec and setup the stack.
   */
  template <class C>
  unsigned  create_ec_helper(C * tls, unsigned cpunr, unsigned excbase, Utcb **utcb_out=0, void *func=0, unsigned cap = alloc_cap())
  {
    unsigned stack_top = stack_size/sizeof(void *);
    Utcb *utcb = alloc_utcb();
    void **stack = new(stack_size) void *[stack_top];
    stack[--stack_top] = utcb; // push UTCB -- needed for myutcb()
    stack[--stack_top] = reinterpret_cast<void *>(0xDEAD);
    stack[--stack_top] = utcb; // push UTCB -- as function parameter
    stack[--stack_top] = tls;
    stack[--stack_top] = func ? func : reinterpret_cast<void *>(idc_reply_and_wait_fast);
    //Logging::printf("\t\tcreate ec[%x,%x] stack %p utcb %p at %p = %p tls %p\n",
		//    cpunr, cap, stack, utcb, stack + stack_top, stack[stack_top], tls);
    check1(0, nova_create_ec(cap, utcb,  stack + stack_top, cpunr, excbase, !func));
    utcb->head.nul_cpunr = cpunr;
    utcb->head.crd = utcb->head.crd_translate = 0;
    if (utcb_out)
      *utcb_out = utcb;
    return cap;
  }


  /**
   * Initialize ourself.
   */
  unsigned init(Hip *hip)
  {
    check1(1, hip->calc_checksum());
    _hip = hip;

    // search for the cpunr of the BSP.
    for (int i=0; i < (_hip->mem_offs - _hip->cpu_offs) / _hip->cpu_size; i++) {
      Hip_cpu *cpu = reinterpret_cast<Hip_cpu *>(reinterpret_cast<char *>(_hip) + _hip->cpu_offs + i*_hip->cpu_size);
      if ((cpu->flags & 3) == 3) {
        myutcb()->head.nul_cpunr = i;
        break;
      }
    }

    // add caps
    assert(!CapAllocator<NovaProgram>::_cap_ && !CapAllocator<NovaProgram>::_cap_start);
    CapAllocator<NovaProgram>::_cap_order = 16;
    assert((1 << 16) < hip->cfg_cap);
    //more than 1 << 20 caps can't be expressed in a CRD currently - even if the hypervisor specifies more to support in cfg_cap
    _cap_region.add(Region(1 << 16, (hip->cfg_cap < (1 << 20) ? hip->cfg_cap : (1 << 20)) - (1 << 16)));

    // reserve cap range where parent pd potentially placed some inital caps for this pd
    unsigned cap_reserved = alloc_cap(1 << Config::CAP_RESERVED_ORDER);
    unsigned res;
    assert(!cap_reserved);
    res = nova_create_sm(_cap_block = alloc_cap());
    assert(res == NOVA_ESUCCESS);

    // add all memory, this does not include the boot_utcb, the HIP and the kernel!
    _free_virt.add(Region(VIRT_START, reinterpret_cast<unsigned long>(reinterpret_cast<Utcb *>(hip) - 1) - VIRT_START));
    _free_virt.del(Region(reinterpret_cast<unsigned long>(&__image_start), &__image_end - &__image_start));

    return 0;
  };


  /**
   * Init the memory map from the HIP as we get them from sigma0.
   */
  void init_mem(Hip *hip) {

    for (int i=0; i < (_hip->length - _hip->mem_offs) / _hip->mem_size; i++) {

      Hip_mem *hmem = reinterpret_cast<Hip_mem *>(reinterpret_cast<char *>(_hip) + _hip->mem_offs) + i;
      if (hmem->type == 1) {
        _free_virt.del(Region(hmem->addr, hmem->size));
        _free_phys.add(Region(hmem->addr, hmem->size, hmem->aux));
        _virt_phys.add(Region(hmem->addr, hmem->size, hmem->aux));
      }
    }

    _free_phys.del(Region(reinterpret_cast<unsigned long>(&__image_start), &__image_end - &__image_start));
  }


  /**
   * Block ourself.
   */
  void __attribute__((noreturn)) block_forever() { while (1) { unsigned res = nova_semdown(_cap_block); Logging::printf("curiosity: block forever returned %x\n", res); }};


public:
  NovaProgram () : CapAllocator<NovaProgram>(0,0,0) {}

  /**
   * Default exit function.
   */
  static void exit(const char *msg)
  {
    Logging::printf("%s() - %s\n", __func__, msg);
  }
};


/**
 * Define the functions needed by asm.s.
 * Unfortunately this can not be a template.
 */
#define ASMFUNCS(X, Y)							\
  extern "C" void start(Hip *hip, Utcb *utcb) __attribute__((regparm(2))) __attribute__((noreturn)); \
  void start(Hip *hip, Utcb *utcb)					\
  {									\
    static X x;								\
    x.run(utcb, hip);							\
    do_exit("run returned");						\
  }									\
  void do_exit(const char *msg)						\
  {									\
    Y::exit(msg);							\
    while (1)								\
      asm volatile ("ud2a" : : "a"(msg));				\
  }

/**
 * The startup function and the initial stack.  Called with the HIP in
 * %esp and the UTCB a page below the HIP.
 */
asm volatile (".global __start;"
	      ".section .text.__start;"
	      "__start:;"
	      "mov	$stack, %eax;"
	      "xchg	%eax, %esp;"
	      "mov	%eax, %edx;"
	      "sub	$0x1000, %edx;"
              "push     %edx;"           // push UTCB -- needed for myutcb()
              "push     $0;"
	      "push	%edx;"           // push UTCB -- as function parameter
	      "pushl	$0;"             // tls
	      "call	start;"
	      "ud2a;"
	      ".section .bss.stack;"
	      ".align 0x1000;"
	      ".space 0x1000;"
	      "stack:;"
	      );

/**
 * A fast reply to our client, called by a return to a portal
 * function.
 */
asm volatile (".section .text.idc_reply_and_wait_fast;"
	      ".global idc_reply_and_wait_fast;"
	      "idc_reply_and_wait_fast:;"
	      // w0: NOVA_IPC_REPLY
	      "mov	$1, %al;"
	      // keep a pointer to ourself on the stack
	      // ecx: stack
	      "lea	-4(%esp), %ecx;"
	      "sysenter;"
	      );