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
#include "esdm_rpc_client_dispatcher.h"
#include "helper.h"
#include "logger.h"
#include "visibility.h"

struct esdm_rnd_add_entropy_buf {
	protobuf_c_boolean is_done;
	int ret;
};

static void
esdm_rpcc_rnd_add_entropy_cb(const RndAddEntropyResponse *response,
			     void *closure_data)
{
	struct esdm_rnd_add_entropy_buf *buffer =
			(struct esdm_rnd_add_entropy_buf *)closure_data;

	if (!response) {
		logger(LOGGER_DEBUG, LOGGER_C_RPC,
		       "missing data - connection interrupted\n");
		buffer->ret = -EINTR;
		goto out;
	}

	buffer->ret = response->ret;

out:
	buffer->is_done = 1;
}

DSO_PUBLIC
int esdm_rpcc_rnd_add_entropy(const uint8_t *entropy_buf,
			      size_t entropy_buf_len,
			      uint32_t entropy_cnt)
{
	RndAddEntropyRequest msg = RND_ADD_ENTROPY_REQUEST__INIT;
	struct esdm_dispatcher *disp;
	struct esdm_rnd_add_entropy_buf buffer;
	int ret = 0;

	ret = esdm_disp_get_priv(&disp);
	if (ret)
		return ret;

	buffer.is_done = 0;
	buffer.ret = -ETIMEDOUT;

	//TODO unconstify
	msg.randval.data = (uint8_t *)entropy_buf;
	msg.randval.len = entropy_buf_len;
	msg.entcnt = entropy_cnt;

	priv_access__rpc_rnd_add_entropy(disp->service, &msg,
					 esdm_rpcc_rnd_add_entropy_cb, &buffer);
	while (!buffer.is_done)
		protobuf_c_rpc_dispatch_run(disp->dispatch);

	ret = buffer.ret;

	esdm_disp_put_priv(disp);
	return ret;
}
