/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2020 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Atish Patra <atish.patra@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_barrier.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_atomic.h>
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_hsm.h>
#include <sbi/sbi_init.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_system.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_console.h>

static unsigned long hart_data_offset;

/** Per hart specific data to manage state transition **/
struct sbi_hsm_data {
	atomic_t state;
};

int sbi_hsm_hart_state_to_status(int state)
{
	int ret;

	switch (state) {
	case SBI_HART_STOPPED:
		ret = SBI_HSM_HART_STATUS_STOPPED;
		break;
	case SBI_HART_STOPPING:
		ret = SBI_HSM_HART_STATUS_STOP_PENDING;
		break;
	case SBI_HART_STARTING:
		ret = SBI_HSM_HART_STATUS_START_PENDING;
		break;
	case SBI_HART_STARTED:
		ret = SBI_HSM_HART_STATUS_STARTED;
		break;
	default:
		ret = SBI_EINVAL;
	}

	return ret;
}

static inline int __sbi_hsm_hart_get_state(u32 hartid)
{
	struct sbi_hsm_data *hdata;
	struct sbi_scratch *scratch;

	scratch = sbi_hartid_to_scratch(hartid);
	if (!scratch)
		return SBI_HART_UNKNOWN;

	hdata = sbi_scratch_offset_ptr(scratch, hart_data_offset);
	return atomic_read(&hdata->state);
}

int sbi_hsm_hart_get_state(const struct sbi_domain *dom, u32 hartid)
{
	if (!sbi_domain_is_assigned_hart(dom, hartid))
		return SBI_HART_UNKNOWN;

	return __sbi_hsm_hart_get_state(hartid);
}

static bool sbi_hsm_hart_started(const struct sbi_domain *dom, u32 hartid)
{
	if (sbi_hsm_hart_get_state(dom, hartid) == SBI_HART_STARTED)
		return TRUE;
	else
		return FALSE;
}

/**
 * Get ulong HART mask for given HART base ID
 * @param dom the domain to be used for output HART mask
 * @param hbase the HART base ID
 * @param out_hmask the output ulong HART mask
 * @return 0 on success and SBI_Exxx (< 0) on failure
 * Note: the output HART mask will be set to zero on failure as well.
 */
int sbi_hsm_hart_started_mask(const struct sbi_domain *dom,
			      ulong hbase, ulong *out_hmask)
{
	ulong i, hmask, dmask;
	ulong hend = sbi_scratch_last_hartid() + 1;

	*out_hmask = 0;
	if (hend <= hbase)
		return SBI_EINVAL;
	if (BITS_PER_LONG < (hend - hbase))
		hend = hbase + BITS_PER_LONG;

	dmask = sbi_domain_get_assigned_hartmask(dom, hbase);
	for (i = hbase; i < hend; i++) {
		hmask = 1UL << (i - hbase);
		if ((dmask & hmask) &&
		    (__sbi_hsm_hart_get_state(i) == SBI_HART_STARTED))
			*out_hmask |= hmask;
	}

	return 0;
}

void sbi_hsm_prepare_next_jump(struct sbi_scratch *scratch, u32 hartid)
{
	u32 oldstate;
	struct sbi_hsm_data *hdata = sbi_scratch_offset_ptr(scratch,
							    hart_data_offset);

	oldstate = atomic_cmpxchg(&hdata->state, SBI_HART_STARTING,
				  SBI_HART_STARTED);
	if (oldstate != SBI_HART_STARTING)
		sbi_hart_hang();
}

static void sbi_hsm_hart_wait(struct sbi_scratch *scratch, u32 hartid)
{
	unsigned long saved_mie;
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);
	struct sbi_hsm_data *hdata = sbi_scratch_offset_ptr(scratch,
							    hart_data_offset);
	/* Save MIE CSR */
	saved_mie = csr_read(CSR_MIE);

	/* Set MSIE bit to receive IPI */
	csr_set(CSR_MIE, MIP_MSIP);

	/* Wait for hart_add call*/
	while (atomic_read(&hdata->state) != SBI_HART_STARTING) {
		wfi();
	};

	/* Restore MIE CSR */
	csr_write(CSR_MIE, saved_mie);

	/* Clear current HART IPI */
	sbi_platform_ipi_clear(plat, hartid);
}

int sbi_hsm_init(struct sbi_scratch *scratch, u32 hartid, bool cold_boot)
{
	u32 i;
	struct sbi_scratch *rscratch;
	struct sbi_hsm_data *hdata;

	if (cold_boot) {
		hart_data_offset = sbi_scratch_alloc_offset(sizeof(*hdata),
							    "HART_DATA");
		if (!hart_data_offset)
			return SBI_ENOMEM;

		/* Initialize hart state data for every hart */
		for (i = 0; i <= sbi_scratch_last_hartid(); i++) {
			rscratch = sbi_hartid_to_scratch(i);
			if (!rscratch)
				continue;

			hdata = sbi_scratch_offset_ptr(rscratch,
						       hart_data_offset);
			ATOMIC_INIT(&hdata->state,
			(i == hartid) ? SBI_HART_STARTING : SBI_HART_STOPPED);
		}
	} else {
		sbi_hsm_hart_wait(scratch, hartid);
	}

	return 0;
}

void __noreturn sbi_hsm_exit(struct sbi_scratch *scratch)
{
	u32 hstate;
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);
	struct sbi_hsm_data *hdata = sbi_scratch_offset_ptr(scratch,
							    hart_data_offset);
	void (*jump_warmboot)(void) = (void (*)(void))scratch->warmboot_addr;

	hstate = atomic_cmpxchg(&hdata->state, SBI_HART_STOPPING,
				SBI_HART_STOPPED);
	if (hstate != SBI_HART_STOPPING)
		goto fail_exit;

	if (sbi_platform_has_hart_hotplug(plat)) {
		sbi_platform_hart_stop(plat);
		/* It should never reach here */
		goto fail_exit;
	}

	/**
	 * As platform is lacking support for hotplug, directly jump to warmboot
	 * and wait for interrupts in warmboot. We do it preemptively in order
	 * preserve the hart states and reuse the code path for hotplug.
	 */
	jump_warmboot();

fail_exit:
	/* It should never reach here */
	sbi_printf("ERR: Failed stop hart [%u]\n", current_hartid());
	sbi_hart_hang();
}

int sbi_hsm_hart_start(struct sbi_scratch *scratch,
		       const struct sbi_domain *dom,
		       u32 hartid, ulong saddr, ulong smode, ulong priv)
{
	unsigned long init_count;
	unsigned int hstate;
	struct sbi_scratch *rscratch;
	struct sbi_hsm_data *hdata;
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);

	/* For now, we only allow start mode to be S-mode or U-mode. */
	if (smode != PRV_S && smode != PRV_U)
		return SBI_EINVAL;
	if (dom && !sbi_domain_is_assigned_hart(dom, hartid))
		return SBI_EINVAL;
	if (dom && !sbi_domain_check_addr(dom, saddr, smode,
					  SBI_DOMAIN_EXECUTE))
		return SBI_EINVAL;

	rscratch = sbi_hartid_to_scratch(hartid);
	if (!rscratch)
		return SBI_EINVAL;
	hdata = sbi_scratch_offset_ptr(rscratch, hart_data_offset);
	hstate = atomic_cmpxchg(&hdata->state, SBI_HART_STOPPED,
				SBI_HART_STARTING);
	if (hstate == SBI_HART_STARTED)
		return SBI_EALREADY;

	/**
	 * if a hart is already transition to start or stop, another start call
	 * is considered as invalid request.
	 */
	if (hstate != SBI_HART_STOPPED)
		return SBI_EINVAL;

	init_count = sbi_init_count(hartid);
	rscratch->next_arg1 = priv;
	rscratch->next_addr = saddr;
	rscratch->next_mode = smode;

	if (sbi_platform_has_hart_hotplug(plat) ||
	   (sbi_platform_has_hart_secondary_boot(plat) && !init_count)) {
		return sbi_platform_hart_start(plat, hartid,
					       scratch->warmboot_addr);
	} else {
		sbi_platform_ipi_send(plat, hartid);
	}

	return 0;
}

int sbi_hsm_hart_stop(struct sbi_scratch *scratch, bool exitnow)
{
	int oldstate;
	u32 hartid = current_hartid();
	struct sbi_hsm_data *hdata = sbi_scratch_offset_ptr(scratch,
							    hart_data_offset);

	if (!sbi_hsm_hart_started(sbi_domain_thishart_ptr(), hartid))
		return SBI_EINVAL;

	oldstate = atomic_cmpxchg(&hdata->state, SBI_HART_STARTED,
				  SBI_HART_STOPPING);
	if (oldstate != SBI_HART_STARTED) {
		sbi_printf("%s: ERR: The hart is in invalid state [%u]\n",
			   __func__, oldstate);
		return SBI_EDENIED;
	}

	if (exitnow)
		sbi_exit(scratch);

	return 0;
}

int sbi_hsm_hart_stop_remote(struct sbi_scratch *scratch, u32 hartid)
{
	struct sbi_scratch *rscratch;
	struct sbi_hsm_data *hdata;
	u32 hstate;

	rscratch = sbi_hartid_to_scratch(hartid);
	if (!rscratch)
		return SBI_EINVAL;

	hdata = sbi_scratch_offset_ptr(rscratch, hart_data_offset);
	hstate = atomic_xchg(&hdata->state, SBI_HART_STOPPING);
	if (hstate != SBI_HART_STARTED && hstate != SBI_HART_STOPPING \
			&& hstate != SBI_HART_STOPPED)
		sbi_printf("WARNING: unexpect state: %d\n", hstate);

	return 0;
}

void sbi_hsm_hart_maybe_switch_to_stopped(void)
{
	u32 hartid = current_hartid();
	struct sbi_scratch *scratch;
	struct sbi_hsm_data *hdata;
	u32 hstate;

	scratch = sbi_hartid_to_scratch(hartid);
	if (!scratch)
		return;

	hdata = sbi_scratch_offset_ptr(scratch, hart_data_offset);
	hstate = atomic_read(&hdata->state);
	if (SBI_HART_STOPPING == hstate)
		sbi_exit(scratch);
}
