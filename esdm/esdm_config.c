/* ESDM Runtime configuration facility
 *
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 *
 * License: see LICENSE file in root directory
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "build_bug_on.h"
#include "config.h"
#include "esdm_config.h"
#include "esdm_definitions.h"
#include "esdm_es_mgr.h"
#include "fips.h"
#include "helper.h"
#include "logger.h"
#include "visibility.h"

struct esdm_config {
	uint32_t esdm_es_cpu_entropy_rate_bits;
	uint32_t esdm_es_jent_entropy_rate_bits;
	uint32_t esdm_es_krng_entropy_rate_bits;
	uint32_t esdm_es_sched_entropy_rate_bits;
	uint32_t esdm_drng_max_wo_reseed;
	uint32_t esdm_max_nodes;
	enum esdm_config_force_fips force_fips;
};

static struct esdm_config esdm_config = {
	/*
	 * Estimated entropy of data is a 32th of
	 * ESDM_DRNG_SECURITY_STRENGTH_BITS. As we have no ability to review the
	 * implementation of those noise sources, it is prudent to have a
	 * conservative estimate here.
	 */
	.esdm_es_cpu_entropy_rate_bits = ESDM_CPU_ENTROPY_RATE,

	/*
	 * Estimated entropy of data is a 16th of
	 * ESDM_DRNG_SECURITY_STRENGTH_BITS. Albeit a full entropy assessment
	 * is provided for the noise source indicating that it provides high
	 * entropy rates and considering that it deactivates when it detects
	 * insufficient hardware, the chosen under estimation of entropy is
	 * considered to be acceptable to all reviewers.
	 */
	.esdm_es_jent_entropy_rate_bits = ESDM_JENT_ENTROPY_RATE,

	/*
	 * See documentation of ESDM_KERNEL_RNG_ENTROPY_RATE
	 */
	.esdm_es_krng_entropy_rate_bits = ESDM_KERNEL_RNG_ENTROPY_RATE,

	/*
	 * See documentation of ESDM_SCHED_ENTROPY_RATE
	 */
	.esdm_es_sched_entropy_rate_bits = ESDM_SCHED_ENTROPY_RATE,

	/*
	 * See documentation of ESDM_DRNG_MAX_WITHOUT_RESEED.
	 */
	.esdm_drng_max_wo_reseed = ESDM_DRNG_MAX_WITHOUT_RESEED,

	/*
	 * Upper limit of DRNG nodes
	 */
	.esdm_max_nodes = 0xffffffff,

	/* Shall the FIPS mode be forcefully set/unset? */
	.force_fips = esdm_config_force_fips_unset,
};

static uint32_t esdm_config_entropy_rate_max(uint32_t val)
{
	return min_t(uint32_t, ESDM_DRNG_SECURITY_STRENGTH_BITS, val);
}

DSO_PUBLIC
uint32_t esdm_config_es_cpu_entropy_rate(void)
{
	return esdm_config.esdm_es_cpu_entropy_rate_bits;
}

DSO_PUBLIC
void esdm_config_es_cpu_entropy_rate_set(uint32_t ent)
{
	esdm_config.esdm_es_cpu_entropy_rate_bits =
		esdm_config_entropy_rate_max(ent);
	esdm_es_add_entropy();
}

DSO_PUBLIC
uint32_t esdm_config_es_jent_entropy_rate(void)
{
	return esdm_config.esdm_es_jent_entropy_rate_bits;
}

DSO_PUBLIC
void esdm_config_es_jent_entropy_rate_set(uint32_t ent)
{
	esdm_config.esdm_es_jent_entropy_rate_bits =
		esdm_config_entropy_rate_max(ent);
	esdm_es_add_entropy();
}

DSO_PUBLIC
uint32_t esdm_config_es_krng_entropy_rate(void)
{
	return esdm_config.esdm_es_krng_entropy_rate_bits;
}

DSO_PUBLIC
void esdm_config_es_krng_entropy_rate_set(uint32_t ent)
{
	esdm_config.esdm_es_krng_entropy_rate_bits =
		esdm_config_entropy_rate_max(ent);
	esdm_es_add_entropy();
}

DSO_PUBLIC
uint32_t esdm_config_es_sched_entropy_rate(void)
{
	return esdm_config.esdm_es_sched_entropy_rate_bits;
}

DSO_PUBLIC
void esdm_config_es_sched_entropy_rate_set(uint32_t ent)
{
	esdm_config.esdm_es_sched_entropy_rate_bits =
		esdm_config_entropy_rate_max(ent);
	esdm_es_add_entropy();
}

DSO_PUBLIC
uint32_t esdm_config_drng_max_wo_reseed(void)
{
	/* If DRNG operated without proper reseed for too long, block ESDM */
	BUILD_BUG_ON(ESDM_DRNG_MAX_WITHOUT_RESEED < ESDM_DRNG_RESEED_THRESH);
	return esdm_config.esdm_drng_max_wo_reseed;
}

DSO_PUBLIC
uint32_t esdm_config_max_nodes(void)
{
	return esdm_config.esdm_max_nodes;
}

#ifdef ESDM_TESTMODE
void esdm_config_drng_max_wo_reseed_set(uint32_t val)
{
	esdm_config.esdm_drng_max_wo_reseed = val;
}

void esdm_config_max_nodes_set(uint32_t val)
{
	esdm_config.esdm_max_nodes = val;
}
#endif

/******************************************************************************/

DSO_PUBLIC
void esdm_config_force_fips_set(enum esdm_config_force_fips val)
{
	esdm_config.force_fips = val;
}

DSO_PUBLIC
int esdm_config_fips_enabled(void)
{
	if (esdm_config.force_fips == esdm_config_force_fips_unset)
		return fips_enabled();
	return (esdm_config.force_fips == esdm_config_force_fips_enabled);
}

DSO_PUBLIC
uint32_t esdm_config_online_nodes(void)
{
	return min_t(uint32_t, esdm_online_nodes(), esdm_config_max_nodes());
}

DSO_PUBLIC
uint32_t esdm_config_curr_node(void)
{
	return esdm_curr_node() % esdm_config_max_nodes();
}

int esdm_config_init(void)
{

	/*
	 * Sanity checks - if runtime configuration is added, it must be
	 * above these checks.
	 */
	esdm_config.esdm_es_cpu_entropy_rate_bits =
		esdm_config_entropy_rate_max(
			esdm_config.esdm_es_cpu_entropy_rate_bits);
	esdm_config.esdm_es_jent_entropy_rate_bits =
		esdm_config_entropy_rate_max(
			esdm_config.esdm_es_jent_entropy_rate_bits);
	esdm_config.esdm_es_krng_entropy_rate_bits =
		esdm_config_entropy_rate_max(
			esdm_config.esdm_es_krng_entropy_rate_bits);
	esdm_config.esdm_es_sched_entropy_rate_bits =
		esdm_config_entropy_rate_max(
			esdm_config.esdm_es_sched_entropy_rate_bits);

	/*
	 * In FIPS mode, the Jitter RNG is defined to have full of entropy
	 * unless a different value has been specified at the command line
	 * (i.e. the user overrides the default), and the default value is
	 * larger than zero (if it is zero, it is assumed that an RBG2(P) or
	 * RBG2(NP) construction is attempted that intends to exclude the
	 * Jitter RNG).
	 */
	if (esdm_config_fips_enabled() &&
	    ESDM_JENT_ENTROPY_RATE > 0 &&
	    esdm_config_es_jent_entropy_rate() ==
	     ESDM_JENT_ENTROPY_RATE)
		esdm_config_es_jent_entropy_rate_set(
			ESDM_DRNG_SECURITY_STRENGTH_BITS);

	return 0;
}