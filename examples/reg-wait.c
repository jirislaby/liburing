/* SPDX-License-Identifier: MIT */
/*
 * Sample program that shows how to use registered waits.
 *
 * (C) 2024 Jens Axboe <axboe@kernel.dk>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <liburing.h>

static unsigned long long mtime_since(const struct timeval *s,
				      const struct timeval *e)
{
	long long sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	sec *= 1000;
	usec /= 1000;
	return sec + usec;
}

static unsigned long long mtime_since_now(struct timeval *tv)
{
	struct timeval end;

	gettimeofday(&end, NULL);
	return mtime_since(tv, &end);
}

int main(int argc, char *argv[])
{
	struct io_uring_reg_wait *reg;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe[2];
	struct io_uring ring;
	char b1[8], b2[8];
	unsigned long msec;
	struct timeval tv;
	int ret, fds[2];

	if (argc > 1) {
		fprintf(stdout, "%s: takes no arguments\n", argv[0]);
		return 0;
	}

	if (pipe(fds) < 0) {
		perror("pipe");
		return 1;
	}

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "Queue init: %d\n", ret);
		return 1;
	}

	/*
	 * Setup wait region. We'll use 32 here, but 64 is probably a more
	 * logical value, as it'll pin a page regardless of size. 64 is the
	 * max value on a 4k page size architecture.
	 */
	reg = io_uring_setup_reg_wait(&ring, 32, &ret);
	if (!reg) {
		if (ret == -EINVAL) {
			fprintf(stderr, "Kernel doesn't support registered waits\n");
			return 1;
		}
		fprintf(stderr, "Registered wait: %d\n", ret);
		return 1;
	}

	/*
	 * Setup two distinct wait regions. Index 0 will be a 1 second wait,
	 * and region 2 is a short wait using min_wait_usec as well. Neither
	 * of these use a signal mask, but sigmask/sigmask_sz can be set as
	 * well for that.
	 */
	reg[0].ts.tv_sec = 1;
	reg[0].ts.tv_nsec = 0;
	reg[0].flags = IORING_REG_WAIT_TS;

	reg[1].ts.tv_sec = 0;
	reg[1].ts.tv_nsec = 100000000LL;
	reg[1].min_wait_usec = 10000;
	reg[1].flags = IORING_REG_WAIT_TS;

	/*
	 * No pending completions. Wait with region 0, which should time
	 * out after 1 second.
	 */
	gettimeofday(&tv, NULL);
	ret = io_uring_submit_and_wait_reg(&ring, cqe, 1, 0);
	if (ret == -EINVAL) {
		fprintf(stderr, "Kernel doesn't support registered waits\n");
		return 1;
	} else if (ret != -ETIME) {
		fprintf(stderr, "Wait should've timed out... %d\n", ret);
		return 1;
	}
	msec = mtime_since_now(&tv);
	if (msec < 900 || msec > 1100) {
		fprintf(stderr, "Wait took an unexpected amount of time: %lu\n",
			msec);
		return 1;
	}

	/*
	 * Now prepare two pipe reads. We'll trigger one completion quickly,
	 * but the other one will never happen. Use min_wait_usec timeout
	 * to abort after 10 msec of time, where the overall timeout is
	 * otherwise 100 msec. Since we're waiting on two events, the min
	 * timeout ends up aborting us.
	 */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, fds[0], b1, sizeof(b1), 0);
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, fds[0], b2, sizeof(b2), 0);

	/* trigger one read */
	ret = write(fds[1], "Hello", 5);
	if (ret < 0) {
		perror("write");
		return 1;
	}

	/*
	 * This should will wait for 2 entries, where 1 is already available.
	 * Since we're using min_wait_usec == 10 msec here with an overall
	 * wait of 100 msec, we expect the wait to abort after 10 msec since
	 * one or more events are available.
	 */
	gettimeofday(&tv, NULL);
	ret = io_uring_submit_and_wait_reg(&ring, cqe, 2, 1);
	msec = mtime_since_now(&tv);
	if (ret != 2) {
		fprintf(stderr, "Should have submitted 2: %d\n", ret);
		return 1;
	}
	if (msec < 8 || msec > 12)
		fprintf(stderr, "min_wait_usec should take ~10 msec: %lu\n", msec);

	/*
	 * Cleanup after ourselves
	 */
	io_uring_queue_exit(&ring);
	io_uring_free_reg_wait(reg, 32);
	return 0;
}