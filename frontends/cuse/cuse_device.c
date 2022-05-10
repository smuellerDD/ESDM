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

#define _GNU_SOURCE
#include <errno.h>
#include <linux/random.h>
#include <poll.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/shm.h>
#include <unistd.h>

#include "bool.h"
#include "cuse_device.h"
#include "esdm_rpc_client_connection.h"
#include "esdm_rpc_client_dispatcher.h"
#include "esdm_rpc_service.h"
#include "helper.h"
#include "logger.h"
#include "memset_secure.h"
#include "mutex_w.h"
#include "privileges.h"
#include "selinux.h"
#include "ret_checkers.h"
#include "threading_support.h"

/******************************************************************************
 * Bind mount handling code
 ******************************************************************************/

static char *mount_src = NULL;
static char *mount_dst = NULL;

static int esdm_cuse_bind_mount(void)
{
	/* This is only to shut up valgrind */
	static const char type[] = "bind";

	if (!mount_dst || !mount_src)
		return -EFAULT;

	if (mount(mount_src, mount_dst, type, MS_BIND, NULL) < 0) {
		int errsv = errno;

		logger(LOGGER_ERR, LOGGER_C_CUSE,
		       "Failed to created bind mount from %s to %s\n",
		       mount_src, mount_dst);
		return -errsv;
	}

	if (esdm_cuse_restore_label(mount_dst) < 0) {
		int errsv = errno;

		umount(mount_dst);
		logger(LOGGER_ERR, LOGGER_C_CUSE, "Failed properly relabel %s\n",
		       mount_dst);
		return -errsv;
	}

	logger(LOGGER_VERBOSE, LOGGER_C_CUSE,
	       "Successfully created bind mount from %s to %s\n",
	       mount_src, mount_dst);
	return 0;
}

static int esdm_cuse_bind_unmount(void)
{
#define MAX_WAIT_SEC (8 * 5)
	struct timespec sleep = { 0, 1 << 27 };
	unsigned int ctr = 0;
	int ret, errsv;

	if (!mount_dst)
		return 0;

	ret = raise_privilege_transient(0, 0);
	if (ret < 0) {
		logger(LOGGER_WARN, LOGGER_C_CUSE,
		       "Failed to raise privilege for unmount bind mount\n");
		return ret;
	}

	do {
		errsv = 0;
		ret = umount(mount_dst);
		if (ret < 0 && errno == EBUSY) {
			errsv = errno;
			nanosleep(&sleep, NULL);
			ctr++;
		}
	} while (ret < 0 && errsv == EBUSY && ctr < MAX_WAIT_SEC);

	if (ret < 0) {
		errsv = errno;
		logger(LOGGER_WARN, LOGGER_C_CUSE,
		       "Failed to remove bind mount from %s\n", mount_dst);
		ret = -errsv;
	} else {
		logger(LOGGER_DEBUG, LOGGER_C_CUSE,
		       "Successfully removed bind mount from %s\n", mount_dst);
	}

	if (mount_src)
		free(mount_src);
	mount_src = NULL;
	if (mount_dst)
		free(mount_dst);
	mount_dst = NULL;

	return ret;
}

/******************************************************************************
 * Semaphore for shared memory segment
 ******************************************************************************/

static sem_t *esdm_cuse_semid = SEM_FAILED;

static void esdm_cuse_shm_status_down(void)
{
	if (esdm_cuse_semid == SEM_FAILED)
		return;

	if (sem_wait(esdm_cuse_semid))
		logger(LOGGER_ERR, LOGGER_C_CUSE, "Cannot use semaphore\n");
}


static void esdm_cuse_shm_status_close_sem(void)
{
	if (esdm_cuse_semid != SEM_FAILED) {
		sem_close(esdm_cuse_semid);
		esdm_cuse_semid = SEM_FAILED;
	}
}

static int esdm_cuse_shm_status_create_sem(void)
{
	int errsv;

	esdm_cuse_semid = sem_open(ESDM_SEM_NAME, O_CREAT, 0644, 0);
	if (esdm_cuse_semid == SEM_FAILED) {
		errsv = errno;
		logger(LOGGER_ERR, LOGGER_C_CUSE,
		       "Semaphore creation failed: %s\n",
		       strerror(errsv));
		return -errsv;
	}

	logger(LOGGER_DEBUG, LOGGER_C_CUSE,
	       "ESDM change indicator semaphore successfully attached to\n");

	return 0;
}

/******************************************************************************
 * Shared memory segment
 ******************************************************************************/

static struct esdm_shm_status *esdm_cuse_shm_status = NULL;
static int esdm_cuse_shmid = -1;

static void esdm_cuse_shm_status_close_shm(void)
{
	if (esdm_cuse_shm_status) {
		shmdt(esdm_cuse_shm_status);
		esdm_cuse_shm_status = NULL;
	}
	if (esdm_cuse_shmid >= 0) {
		shmctl(esdm_cuse_shmid, IPC_RMID, NULL);
		esdm_cuse_shmid = -1;
	}
}

static int esdm_cuse_shm_status_create_shm(void)
{
	int errsv;
	void *tmp;

	key_t key = esdm_ftok(ESDM_SHM_NAME, ESDM_SHM_STATUS);

	esdm_cuse_shmid = shmget(key, sizeof(struct esdm_shm_status),
				 S_IRUSR | S_IRGRP | S_IROTH);
	if (esdm_cuse_shmid < 0) {
		errsv = errno;
		logger(LOGGER_ERR, LOGGER_C_CUSE,
		       "Shared memory segment creation failed: %s\n",
		       strerror(errsv));
		return -errsv;
	}

	tmp = shmat(esdm_cuse_shmid, NULL, SHM_RDONLY);
	if (tmp == (void *)-1) {
		errsv = errno;
		logger(LOGGER_ERR, LOGGER_C_CUSE,
		       "Attaching to shared memory segment failed: %s\n",
		       strerror(errsv));
		esdm_cuse_shm_status_close_shm();
		return -errsv;
	}
	esdm_cuse_shm_status = tmp;

	if (esdm_cuse_shm_status->version != ESDM_SHM_STATUS_VERSION) {
		logger(LOGGER_ERR, LOGGER_C_CUSE,
		       "Shared memory segment version mismatch (expected %u, found %u)\n",
		       ESDM_SHM_STATUS_VERSION, esdm_cuse_shm_status->version);
		return -EINVAL;
	}

	logger(LOGGER_DEBUG, LOGGER_C_CUSE,
	       "ESDM shared memory segment successfully attached to\n");

	return 0;
}

/******************************************************************************
 * Signal handler
 ******************************************************************************/

static bool esdm_cuse_poll_thread_shutdown = false;

static void esdm_cuse_term(void)
{
	esdm_cuse_poll_thread_shutdown = true;

	thread_stop_spawning();

	/*
	 * We forcefully kill the SHM monitor thread as most likely it is
	 * waiting in sem_wait.
	 */
	thread_release(true, true);

	esdm_disp_fini_unpriv();
	esdm_disp_fini_priv();

	esdm_cuse_shm_status_close_shm();
	esdm_cuse_shm_status_close_sem();

	/* Return code is irrelevant here */
	esdm_cuse_bind_unmount();
}

/* terminate the daemon cleanly */
static void esdm_cuse_sig_handler(int sig)
{
	esdm_cuse_term();

	signal(SIGABRT, SIG_DFL);
	signal(SIGALRM, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGILL, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGIO, SIG_DFL);
	signal(SIGIOT, SIG_DFL);
	//signal(SIGPIPE, SIG_DFL);
	signal(SIGPOLL, SIG_DFL);
	signal(SIGPROF, SIG_DFL);
	signal(SIGPWR, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGSEGV, SIG_DFL);
	signal(SIGSYS, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGTRAP, SIG_DFL);
	signal(SIGUSR1, SIG_DFL);
	signal(SIGUSR2, SIG_DFL);
	signal(SIGVTALRM, SIG_DFL);
	signal(SIGXCPU, SIG_DFL);
	signal(SIGXFSZ, SIG_DFL);

	exit(sig);
}

static int esdm_cuse_install_sig_handler(void)
{
	logger(LOGGER_DEBUG, LOGGER_C_CUSE,
	       "Install termination signal handler\n");

	/* Catch all termination signals to ensure the bind mount is removed */
	signal(SIGABRT, esdm_cuse_sig_handler);
	signal(SIGALRM, esdm_cuse_sig_handler);
	signal(SIGBUS, esdm_cuse_sig_handler);
	signal(SIGFPE, esdm_cuse_sig_handler);
	signal(SIGHUP, esdm_cuse_sig_handler);
	signal(SIGILL, esdm_cuse_sig_handler);
	signal(SIGINT, esdm_cuse_sig_handler);
	signal(SIGIO, esdm_cuse_sig_handler);
	signal(SIGIOT, esdm_cuse_sig_handler);
	/* SIGPIPE is used as control mechanism by Protobuf-C-RPC */
	//signal(SIGPIPE, esdm_cuse_sig_handler);
	signal(SIGPOLL, esdm_cuse_sig_handler);
	signal(SIGPROF, esdm_cuse_sig_handler);
	signal(SIGPWR, esdm_cuse_sig_handler);
	signal(SIGQUIT, esdm_cuse_sig_handler);
	signal(SIGSEGV, esdm_cuse_sig_handler);
	signal(SIGSYS, esdm_cuse_sig_handler);
	signal(SIGTERM, esdm_cuse_sig_handler);
	signal(SIGTRAP, esdm_cuse_sig_handler);
	signal(SIGUSR1, esdm_cuse_sig_handler);
	signal(SIGUSR2, esdm_cuse_sig_handler);
	signal(SIGVTALRM, esdm_cuse_sig_handler);
	signal(SIGXCPU, esdm_cuse_sig_handler);
	signal(SIGXFSZ, esdm_cuse_sig_handler);

	return 0;
}

/******************************************************************************
 * CUSE helper
 ******************************************************************************/

static bool esdm_cuse_fips_enabled(void)
{
	static char fipsflag[1] = { 'A' };
	size_t n = 0;

	if (fipsflag[0] == 'A') {
#ifdef HAVE_SECURE_GETENV
		if (secure_getenv("ESDM_SERVER_FORCE_FIPS")) {
#else
		if (getenv("ESDM_SERVER_FORCE_FIPS")) {
#endif
			fipsflag[0] = 1;
		} else {
			FILE *fipsfile = NULL;

			fipsfile = fopen("/proc/sys/crypto/fips_enabled", "r");
			if (!fipsfile) {
				if (errno == ENOENT) {
					/* FIPS support not enabled in kernel */
					return 0;
				} else {
					logger(LOGGER_ERR, LOGGER_C_CUSE,
						"FIPS: Cannot open fips_enabled file: %s\n",
						strerror(errno));
					return -EIO;
				}
			}

			n = fread((void *)fipsflag, 1, 1, fipsfile);
			fclose(fipsfile);
			if (n != 1) {
				logger(LOGGER_ERR, LOGGER_C_CUSE,
				       "FIPS: Cannot read FIPS flag\n");
				return false;
			}
		}
	}

	return (fipsflag[0] == '1');
}

static const char *esdm_cuse_unprivileged_user = "nobody";
static void esdm_cuse_drop_privileges(void)
{
	static bool dropped = false;

	if (dropped)
		return;

	if (drop_privileges_transient(esdm_cuse_unprivileged_user) == 0)
		dropped = true;
}

static bool esdm_cuse_client_privileged(fuse_req_t req)
{
	const struct fuse_ctx *ctx = fuse_req_ctx(req);

	/*
	 * We are not checking the GID as we expect a root user to use any
	 * GID.
	 *
	 * WARNING: as documented for struct fuse_ctx, the CUSE daemon
	 * MUST NOT run in a PID or user namespace.
	 */
	if (ctx->uid == 0) {
		logger(LOGGER_DEBUG, LOGGER_C_CUSE, "CUSE caller privileged\n");
		return true;
	}

	logger(LOGGER_DEBUG, LOGGER_C_CUSE, "CUSE caller unprivileged\n");
	return false;
}

static void esdm_cuse_raise_privilege(fuse_req_t req)
{
	if (esdm_cuse_client_privileged(req))
		raise_privilege_transient(0, 0);
}

/******************************************************************************
 * CUSE callback handler
 ******************************************************************************/

void esdm_cuse_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);
}

void esdm_cuse_read_internal(fuse_req_t req, size_t size, off_t off,
			     struct fuse_file_info *fi,
			     ssize_t (*get)(uint8_t *buf, size_t buflen),
			     int fallback_fd)
{
	uint8_t tmpbuf[ESDM_RPC_MAX_MSG_SIZE];
	size_t cleansize = min_t(size_t, sizeof(tmpbuf), size);
	ssize_t ret = 0;

	(void)off;
	(void)fi;

	if (size > sizeof(tmpbuf)) {
		logger(LOGGER_ERR, LOGGER_C_CUSE,
		       "Due to FUSE limitation, the maximum request size is %u\n",
		       ESDM_RPC_MAX_MSG_SIZE);
		size = sizeof(tmpbuf);
	}

//	while (size)
	{
		size_t todo = min_t(size_t, sizeof(tmpbuf), size);

		esdm_invoke(get(tmpbuf, todo));

		/*
		 * If call to the ESDM server failed, let us fall back to the
		 * fallback file descriptor. Yet, we do not cover for short
		 * reads as this entire CUSE handling is prone to short reads
		 * as outlined below. Thus, the caller needs to handle this
		 * appropriately.
		 */
		if (ret < 0 && fallback_fd > -1) {
			logger(LOGGER_VERBOSE, LOGGER_C_CUSE,
			       "Use fallback to provide data due to RPC error code %zd\n",
			       ret);
			ret = read(fallback_fd, tmpbuf, todo);
		}

		if (ret < 0)
			goto out;
		todo = (size_t)ret;

		/*
		 * This call segfaults when called a 2nd time because req is
		 * freed. Thus, the while loop is currently disabled.
		 *
		 * Thus, we will return a short read here that the caller must
		 * consider. Returning a short read is permissible in VFS and
		 * thus it is no error to apply a short read here.
		 *
		 * The caller may accommodate that with a while loop around
		 * its read system call. Another example when using dd is
		 * the following command that must be used:
		 *
		 * dd if=/dev/esdm of=out count=1 bs=65550 iflag=fullblock
		 */
		ret = fuse_reply_buf(req, (const char *)tmpbuf, todo);


// 		if (ret < 0)
// 			goto out;
//
// 		size -= todo;
	}

out:
	memset_secure(tmpbuf, 0, cleansize);
	if (ret < 0)
		fuse_reply_err(req, (int)-ret);
}

void esdm_cuse_write_internal(fuse_req_t req, const char *buf, size_t size,
			      off_t off, struct fuse_file_info *fi,
			      int fallback_fd)
{
	ssize_t ret;

	(void)fi;
	(void)off;

	esdm_invoke(esdm_rpcc_write_data((const uint8_t *)buf, size));
	if (ret)
		ret = (ssize_t)size;

	/*
	 * If call to the ESDM server failed, let us fall back to the
	 * fallback file descriptor. Yet, we do not cover for short
	 * reads as this entire CUSE handling is prone to short reads
	 * as outlined below. Thus, the caller needs to handle this
	 * appropriately.
	 */
	if (ret < 0 && fallback_fd > -1) {
		logger(LOGGER_VERBOSE, LOGGER_C_CUSE,
			"Use fallback to provide data due to RPC error code %zd\n",
			ret);
		ret = write(fallback_fd, buf, size);
	}

	if (ret < 0)
		fuse_reply_err(req, (int)-ret);
	else
		fuse_reply_write(req, size);
}

void esdm_cuse_ioctl(int backend_fd,
		     fuse_req_t req, unsigned long cmd, void *arg,
		     struct fuse_file_info *fi, unsigned flags,
		     const void *in_buf, size_t in_bufsz,
		     size_t out_bufsz)
{
	const struct rand_pool_info *rpi;
	uint32_t ent_count_bits;
	int ret;

	(void)fi;

	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	switch (cmd) {
	case RNDGETENTCNT:
		if (!out_bufsz) {
			struct iovec iov = { arg, sizeof(ent_count_bits) };

			fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
		} else {
			esdm_invoke(esdm_rpcc_rnd_get_ent_cnt(&ent_count_bits));
			if (ret)
				fuse_reply_err(req, -ret);
			else
				fuse_reply_ioctl(req, 0, &ent_count_bits,
						 sizeof(ent_count_bits));
		}
		break;
	case RNDADDTOENTCNT:
		if (!in_bufsz || in_bufsz < sizeof(ent_count_bits)) {
			struct iovec iov = { arg, sizeof(ent_count_bits) };
			fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
		} else {
			ent_count_bits = *(uint32_t *)in_buf;

			/*
			 * This operation requires privileges. Thus, raise the
			 * privilege level to the same level as the caller has.
			 */
			if (!esdm_cuse_client_privileged(req)) {
				fuse_reply_err(req, EPERM);
				return;
			}
			esdm_cuse_raise_privilege(req);
			esdm_invoke(esdm_rpcc_rnd_add_to_ent_cnt(
				ent_count_bits));
			if (!ret) {
				if (ioctl(backend_fd, RNDADDTOENTCNT,
					  &ent_count_bits) == -1)
					ret = -errno;
			}
			drop_privileges_transient(esdm_cuse_unprivileged_user);
			if (ret)
				fuse_reply_err(req, -ret);
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;
	case RNDADDENTROPY:
		rpi = (const struct rand_pool_info *)in_buf;

		if (in_bufsz < sizeof(struct rand_pool_info)) {
			struct iovec iov = {arg, sizeof(struct rand_pool_info)};

			fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
		} else if (rpi->buf_size < 0) {
			fuse_reply_err(req, EINVAL);
		} else if ((size_t)rpi->buf_size !=
			   in_bufsz - sizeof(struct rand_pool_info)) {
				struct iovec iov = { arg,
					sizeof(struct rand_pool_info) +
					(size_t)rpi->buf_size};

				fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
		} else {
			/*
			 * This operation requires privileges. Thus, raise the
			 * privilege level to the same level as the caller has.
			 */
			if (!esdm_cuse_client_privileged(req)) {
				fuse_reply_err(req, EPERM);
				return;
			}
			esdm_cuse_raise_privilege(req);

			/*
			 * If in FIPS mode, the ESDM treats the kernel RNG
			 * to deliver zero bits of entropy. Thus, we can claim
			 * all entropy ourselves, even though we re-insert
			 * the same entropy value into the kernel. For the
			 * ESDM, there is no double accounting of entropy.
			 */
			esdm_invoke(esdm_rpcc_rnd_add_entropy(
						(const uint8_t *)rpi->buf,
						(size_t)rpi->buf_size,
						esdm_cuse_fips_enabled() ?
						(uint32_t)rpi->entropy_count :
						0));
			if (!ret) {
				if (ioctl(backend_fd, RNDADDENTROPY, rpi) == -1)
					ret = -errno;
			}
			drop_privileges_transient(esdm_cuse_unprivileged_user);
			if (ret)
				fuse_reply_err(req, -ret);
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;
	case RNDZAPENTCNT:
	case RNDCLEARPOOL:
		/*
		 * This operation requires privileges. Thus, raise the
		 * privilege level to the same level as the caller has.
		 */
		if (!esdm_cuse_client_privileged(req)) {
			fuse_reply_err(req, EPERM);
			return;
		}
		esdm_cuse_raise_privilege(req);
		esdm_invoke(esdm_rpcc_rnd_clear_pool());
		if (!ret) {
			if (ioctl(backend_fd, RNDCLEARPOOL) == -1)
				ret = -errno;
		}
		drop_privileges_transient(esdm_cuse_unprivileged_user);
		if (ret)
			fuse_reply_err(req, -ret);
		else
			fuse_reply_ioctl(req, 0, NULL, 0);
		break;
	case RNDRESEEDCRNG:
		/*
		 * This operation requires privileges. Thus, raise the
		 * privilege level to the same level as the caller has.
		 */
		if (!esdm_cuse_client_privileged(req)) {
			fuse_reply_err(req, EPERM);
			return;
		}
		esdm_cuse_raise_privilege(req);
		esdm_invoke(esdm_rpcc_rnd_reseed_crng());
		if (!ret) {
			if (ioctl(backend_fd, RNDRESEEDCRNG) == -1)
				ret = -errno;
		}
		drop_privileges_transient(esdm_cuse_unprivileged_user);
		if (ret)
			fuse_reply_err(req, -ret);
		else
			fuse_reply_ioctl(req, 0, NULL, 0);
		break;

	/* ESDM-specific IOCTL */
	case 42:
		if (out_bufsz < esdm_cuse_shm_status->infolen) {
			struct iovec iov = { arg,
					     esdm_cuse_shm_status->infolen };

			fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
		} else {
			fuse_reply_ioctl(req, 0, esdm_cuse_shm_status->info,
					 esdm_cuse_shm_status->infolen);
		}
		break;

	default:
		fuse_reply_err(req, EINVAL);
	}
}

/******************************************************************************
 * Poll system call handler
 ******************************************************************************/

#define ESDM_CUSE_MAX_PH			16
struct esdm_cuse_poll {
	uint64_t fh;
	struct fuse_pollhandle *ph;
	uint32_t poll_events;
};
static struct esdm_cuse_poll esdm_cuse_polls[ESDM_CUSE_MAX_PH];
static DEFINE_MUTEX_W_UNLOCKED(esdm_cuse_ph_lock);

static void esdm_cuse_set_pollmask(unsigned int request_events,
				   unsigned int *outmask)
{
	*outmask = 0;

	if (atomic_bool_read(&esdm_cuse_shm_status->operational))
		*outmask |= POLLIN | POLLRDNORM;
	if (atomic_bool_read(&esdm_cuse_shm_status->need_entropy))
		*outmask |= POLLOUT | POLLWRNORM;

	*outmask &= request_events;
}

void esdm_cuse_poll(fuse_req_t req, struct fuse_file_info *fi,
		    struct fuse_pollhandle *ph)
{
	unsigned int i, mask;

	if (!fi->poll_events) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	/*
	 * Check current status and return it if it complies with requested
	 * status.
	 */
	esdm_cuse_set_pollmask(fi->poll_events, &mask);
	fuse_reply_poll(req, mask);

	if (!ph)
		return;

	if (mask) {
		fuse_notify_poll(ph);
		fuse_pollhandle_destroy(ph);
		return;
	}

	mutex_w_lock(&esdm_cuse_ph_lock);
	for (i = 0; i < ESDM_CUSE_MAX_PH; i++) {
		if (esdm_cuse_polls[i].fh == fi->fh) {
			if (esdm_cuse_polls[i].ph)
				fuse_pollhandle_destroy(esdm_cuse_polls[i].ph);
			esdm_cuse_polls[i].fh = 0;
			esdm_cuse_polls[i].ph = NULL;
			esdm_cuse_polls[i].poll_events = 0;
		}

		if (esdm_cuse_polls[i].ph)
			continue;

		esdm_cuse_polls[i].fh = fi->fh;
		esdm_cuse_polls[i].ph = ph;
		esdm_cuse_polls[i].poll_events = fi->poll_events;
		break;
	}

	if (i == ESDM_CUSE_MAX_PH)
		fuse_reply_err(req, EBUSY);

	mutex_w_unlock(&esdm_cuse_ph_lock);
}

/* Poll checker handler executed in separate thread */
static int esdm_cuse_poll_checker(void __unused *unused)
{
	unsigned int i, mask;

	/* Clean out the poll status */
	for (i = 0; i < ESDM_CUSE_MAX_PH; i++) {
		esdm_cuse_polls[i].fh = 0;
		esdm_cuse_polls[i].ph = NULL;
		esdm_cuse_polls[i].poll_events = 0;
	}

	while (!esdm_cuse_poll_thread_shutdown) {
		mutex_w_lock(&esdm_cuse_ph_lock);
		for (i = 0; i < ESDM_CUSE_MAX_PH; i++) {
			if (!esdm_cuse_polls[i].ph)
				continue;

			esdm_cuse_set_pollmask(esdm_cuse_polls[i].poll_events,
					       &mask);

			if (!mask)
				continue;

			fuse_notify_poll(esdm_cuse_polls[i].ph);
			fuse_pollhandle_destroy(esdm_cuse_polls[i].ph);
			esdm_cuse_polls[i].fh = 0;
			esdm_cuse_polls[i].ph = NULL;
			esdm_cuse_polls[i].poll_events = 0;
		}
		mutex_w_unlock(&esdm_cuse_ph_lock);

		esdm_cuse_shm_status_down();
	}

	return 0;
}

/******************************************************************************
 * CUSE daemon
 ******************************************************************************/

void esdm_cuse_init_done(void *userdata)
{
	int ret;

	(void)userdata;

	if (mount_src) {
		if (chmod(mount_src, S_IRUSR | S_IWUSR |
				     S_IRGRP | S_IWGRP |
				     S_IROTH | S_IWOTH) < 0) {
			logger(LOGGER_ERR, LOGGER_C_CUSE,
			       "Changing permissions to world-writeable failed: %s",
			       strerror(errno));
		}
	}

	/*
	 * Return code checking is of no help here, because we do not
	 * know what do do with the error.
	 */
	CKINT(esdm_cuse_bind_mount());
	/* error not handled here */

	CKINT(esdm_cuse_shm_status_create_sem());

	esdm_cuse_drop_privileges();
	CKINT(esdm_cuse_shm_status_create_shm());

	CKINT_LOG(thread_start(esdm_cuse_poll_checker, NULL, 0, NULL),
		  "Starting poll-in-reset thread failed: %d\n", ret);

	return;

out:
	esdm_cuse_term();
	exit(-ret);
}

struct esdm_cuse_param {
	unsigned int	major;
	unsigned int	minor;
	char		*dev_name;
	char		*username;
	unsigned int	verbosity;
	int		is_help;
};

#define ESDM_CUSE_OPT(t, p) { t, offsetof(struct esdm_cuse_param, p), 1 }

static const char *usage =
"usage: esdm_cuse [options]\n"
"\n"
"options:\n"
"    --help|-h               print this help message\n"
"    --maj=MAJ|-M MAJ        device major number\n"
"    --min=MIN|-m MIN        device minor number\n"
"    --name=NAME|-n NAME     device name (mandatory)\n"
"    --verbosity=NUM|-v NUM  verbosity level\n"
"    --username=USER|-v USER unprivileged user name (default: \"nobody\")\n"
"    -d   -o debug           enable debug output (implies -f)\n"
"    -f                      foreground operation\n"
"    -s                      disable multi-threaded operation\n"
"\n";

static const struct fuse_opt esdm_cuse_opts[] = {
	ESDM_CUSE_OPT("-M %u",		major),
	ESDM_CUSE_OPT("--maj=%u",	major),
	ESDM_CUSE_OPT("-m %u",		minor),
	ESDM_CUSE_OPT("--min=%u",	minor),
	ESDM_CUSE_OPT("-n %s",		dev_name),
	ESDM_CUSE_OPT("--name=%s",	dev_name),
	ESDM_CUSE_OPT("-v %u",		verbosity),
	ESDM_CUSE_OPT("--verbosity=%u",	verbosity),
	ESDM_CUSE_OPT("-u %s",		username),
	ESDM_CUSE_OPT("-username %s",	username),
	FUSE_OPT_KEY("-h",		0),
	FUSE_OPT_KEY("--help",		0),
	FUSE_OPT_END
};

static int esdm_cuse_process_arg(void *data, const char *arg, int key,
				 struct fuse_args *outargs)
{
	struct esdm_cuse_param *param = data;

	(void)outargs;
	(void)arg;

	switch (key) {
	case 0:
		param->is_help = 1;
		fprintf(stderr, "%s", usage);
		return fuse_opt_add_arg(outargs, "-ho");
	default:
		return 1;
	}
}

int main_common(const char *devname, const char *target,
		const struct cuse_lowlevel_ops *clop,
		int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct esdm_cuse_param param = { 0, 0, NULL, NULL, 1, 0 };
	char dev_name[128] = "DEVNAME=";
	const char *dev_info_argv[] = { dev_name };
	struct cuse_info ci;
	int ret = 1;

	if (fuse_opt_parse(&args, &param, esdm_cuse_opts,
			   esdm_cuse_process_arg)) {
		logger(LOGGER_ERR, LOGGER_C_CUSE, "failed to parse option\n");
		free(param.dev_name);
		goto out;
	}

	logger_set_verbosity(param.verbosity);

	if (!param.is_help) {
		const char *dev_name_p = param.dev_name;

		/*
		 * The param.username is not freed - we allow this slight
		 * imprecise programming for now as the user name must be
		 * available for the duration of this process. Thus, let the
		 * kernel clean it up during program termination. Yet, we
		 * do not leak memory.
		 */
		if (param.username)
			esdm_cuse_unprivileged_user = param.username;

		if (!param.dev_name)
			dev_name_p = devname;

		strncat(dev_name, dev_name_p,
			sizeof(dev_name) - sizeof("DEVNAME="));

		if (target) {
			char devfile[128] = "/dev/";

			strncat(devfile, dev_name_p,
				sizeof(devfile) - sizeof("/dev/"));
			mount_src = strndup(devfile, sizeof(devfile));
			if (!mount_src)
				return -errno;
			mount_dst = strdup(target);
			if (!mount_dst) {
				int errsv = errno;

				free(mount_src);
				mount_src = NULL;
				return -errsv;
			}
		}

		if (param.dev_name)
			free(param.dev_name);
	}

	CKINT_LOG(esdm_disp_init_unpriv(),
		  "Initialization of dispatcher failed\n");
	CKINT_LOG(esdm_disp_init_priv(),
		  "Initialization of dispatcher failed\n");

	/* One thread group */
	CKINT(thread_init(1));

	memset(&ci, 0, sizeof(ci));
	ci.dev_major = 0;
	ci.dev_minor = 0;
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	esdm_cuse_install_sig_handler();
	ret = cuse_lowlevel_main(args.argc, args.argv, &ci, clop, NULL);

out:
	esdm_cuse_term();
	fuse_opt_free_args(&args);
	return ret;
}