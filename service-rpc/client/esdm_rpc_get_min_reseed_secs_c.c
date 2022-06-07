/*
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

#include <errno.h>
#include <stdio.h>

#include "esdm_rpc_client.h"
#include "esdm_rpc_service.h"
#include "helper.h"
#include "logger.h"
#include "ret_checkers.h"
#include "visibility.h"

struct esdm_min_reseed_secs_buf {
	int ret;
	unsigned int seconds;
};

static void esdm_rpcc_get_min_reseed_secs_cb(
	const GetMinReseedSecsResponse *response, void *closure_data)
{
	struct esdm_min_reseed_secs_buf *buffer =
			(struct esdm_min_reseed_secs_buf *)closure_data;

	if (!response) {
		logger(LOGGER_DEBUG, LOGGER_C_RPC,
		       "missing data - connection interrupted\n");
		buffer->ret = -EINTR;
		return;
	}

	buffer->ret = response->ret;
	buffer->seconds = response->seconds;
}

DSO_PUBLIC
int esdm_rpcc_get_min_reseed_secs(unsigned int *seconds)
{
	GetMinReseedSecsRequest msg = GET_MIN_RESEED_SECS_REQUEST__INIT;
	struct esdm_rpc_client_connection *rpc_conn;
	struct esdm_min_reseed_secs_buf buffer;
	int ret = 0;

	CKINT(esdm_rpcc_get_unpriv_service(&rpc_conn));

	buffer.ret = -ETIMEDOUT;

	unpriv_access__rpc_get_min_reseed_secs(&rpc_conn->service, &msg,
				esdm_rpcc_get_min_reseed_secs_cb, &buffer);

	ret = buffer.ret;
	if (seconds)
		*seconds = buffer.seconds;

out:
	return ret;
}