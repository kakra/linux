// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Various unit tests for the "winesync" synchronization primitive driver.
 *
 * Copyright (C) 2021 Zebediah Figura
 */

#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <linux/winesync.h>
#include "../../kselftest_harness.h"

TEST(semaphore_state)
{
	struct winesync_wait_args wait_args;
	struct winesync_sem_args sem_args;
	struct timespec timeout;
	int fd, ret;

	clock_gettime(CLOCK_MONOTONIC, &timeout);

	fd = open("/dev/winesync", O_CLOEXEC | O_RDONLY);
	ASSERT_LE(0, fd);

	sem_args.count = 3;
	sem_args.max = 2;
	sem_args.sem = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_SEM, &sem_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	sem_args.count = 2;
	sem_args.max = 2;
	sem_args.sem = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, sem_args.sem);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, sem_args.count);
	EXPECT_EQ(2, sem_args.max);

	sem_args.count = 0;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, sem_args.count);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, sem_args.count);
	EXPECT_EQ(2, sem_args.max);

	sem_args.count = 1;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOVERFLOW, errno);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, sem_args.count);
	EXPECT_EQ(2, sem_args.max);

	wait_args.timeout = (uintptr_t)&timeout;
	wait_args.objs = (uintptr_t)&sem_args.sem;
	wait_args.count = 1;
	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, sem_args.count);
	EXPECT_EQ(2, sem_args.max);

	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);
	EXPECT_EQ(2, sem_args.max);

	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	sem_args.count = 3;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOVERFLOW, errno);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);
	EXPECT_EQ(2, sem_args.max);

	sem_args.count = 2;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, sem_args.count);
	EXPECT_EQ(2, sem_args.max);

	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);

	sem_args.count = 1;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, sem_args.count);
	EXPECT_EQ(2, sem_args.max);

	ret = ioctl(fd, WINESYNC_IOC_DELETE, &sem_args.sem);
	EXPECT_EQ(0, ret);

	close(fd);
}

TEST_HARNESS_MAIN
