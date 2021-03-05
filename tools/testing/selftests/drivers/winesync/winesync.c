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

TEST(mutex_state)
{
	struct winesync_wait_args wait_args;
	struct winesync_mutex_args mutex_args;
	struct timespec timeout;
	__u32 owner;
	int fd, ret;

	clock_gettime(CLOCK_MONOTONIC, &timeout);

	fd = open("/dev/winesync", O_CLOEXEC | O_RDONLY);
	ASSERT_LE(0, fd);

	mutex_args.owner = 123;
	mutex_args.count = 0;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	mutex_args.owner = 0;
	mutex_args.count = 2;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	mutex_args.owner = 123;
	mutex_args.count = 2;
	mutex_args.mutex = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, mutex_args.mutex);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0;
	ret = ioctl(fd, WINESYNC_IOC_PUT_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 456;
	ret = ioctl(fd, WINESYNC_IOC_PUT_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_PUT_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, mutex_args.count);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_PUT_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_PUT_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);

	wait_args.timeout = (uintptr_t)&timeout;
	wait_args.objs = (uintptr_t)&mutex_args.mutex;
	wait_args.count = 1;
	wait_args.owner = 456;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(456, mutex_args.owner);

	wait_args.owner = 456;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, mutex_args.count);
	EXPECT_EQ(456, mutex_args.owner);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 456;
	ret = ioctl(fd, WINESYNC_IOC_PUT_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, mutex_args.count);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(456, mutex_args.owner);

	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	owner = 0;
	ret = ioctl(fd, WINESYNC_IOC_KILL_OWNER, &owner);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_KILL_OWNER, &owner);
	EXPECT_EQ(0, ret);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(456, mutex_args.owner);

	owner = 456;
	ret = ioctl(fd, WINESYNC_IOC_KILL_OWNER, &owner);
	EXPECT_EQ(0, ret);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, wait_args.index);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_KILL_OWNER, &owner);
	EXPECT_EQ(0, ret);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, wait_args.index);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	ret = ioctl(fd, WINESYNC_IOC_DELETE, &mutex_args.mutex);
	EXPECT_EQ(0, ret);

	mutex_args.owner = 0;
	mutex_args.count = 0;
	mutex_args.mutex = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, mutex_args.mutex);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	ret = ioctl(fd, WINESYNC_IOC_DELETE, &mutex_args.mutex);
	EXPECT_EQ(0, ret);

	close(fd);
}

TEST(wait_any)
{
	struct winesync_mutex_args mutex_args = {0};
	struct winesync_wait_args wait_args = {0};
	struct winesync_sem_args sem_args = {0};
	struct timespec timeout;
	__u32 objs[2], owner;
	int fd, ret;

	clock_gettime(CLOCK_MONOTONIC, &timeout);

	fd = open("/dev/winesync", O_CLOEXEC | O_RDONLY);
	ASSERT_LE(0, fd);

	sem_args.count = 2;
	sem_args.max = 3;
	sem_args.sem = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, sem_args.sem);

	mutex_args.owner = 0;
	mutex_args.count = 0;
	mutex_args.mutex = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, mutex_args.mutex);

	objs[0] = sem_args.sem;
	objs[1] = mutex_args.mutex;

	wait_args.timeout = (uintptr_t)&timeout;
	wait_args.objs = (uintptr_t)objs;
	wait_args.count = 2;
	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);
	EXPECT_EQ((uintptr_t)objs, wait_args.objs);
	EXPECT_EQ(2, wait_args.count);
	EXPECT_EQ(123, wait_args.owner);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, wait_args.index);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	sem_args.count = 1;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);

	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	wait_args.owner = 123;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, wait_args.index);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	wait_args.owner = 456;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_KILL_OWNER, &owner);
	EXPECT_EQ(0, ret);

	wait_args.owner = 456;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(1, wait_args.index);

	wait_args.owner = 456;
	wait_args.index = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, wait_args.index);

	/* test waiting on the same object twice */
	sem_args.count = 2;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);

	objs[0] = objs[1] = sem_args.sem;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, wait_args.index);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	wait_args.count = 0;
	wait_args.objs = (uintptr_t)NULL;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	ret = ioctl(fd, WINESYNC_IOC_DELETE, &sem_args.sem);
	EXPECT_EQ(0, ret);
	ret = ioctl(fd, WINESYNC_IOC_DELETE, &mutex_args.mutex);
	EXPECT_EQ(0, ret);

	close(fd);
}

TEST(wait_all)
{
	struct winesync_mutex_args mutex_args = {0};
	struct winesync_wait_args wait_args = {0};
	struct winesync_sem_args sem_args = {0};
	struct timespec timeout;
	__u32 objs[2], owner;
	int fd, ret;

	clock_gettime(CLOCK_MONOTONIC, &timeout);

	fd = open("/dev/winesync", O_CLOEXEC | O_RDONLY);
	ASSERT_LE(0, fd);

	sem_args.count = 2;
	sem_args.max = 3;
	sem_args.sem = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, sem_args.sem);

	mutex_args.owner = 0;
	mutex_args.count = 0;
	mutex_args.mutex = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, mutex_args.mutex);

	objs[0] = sem_args.sem;
	objs[1] = mutex_args.mutex;

	wait_args.timeout = (uintptr_t)&timeout;
	wait_args.objs = (uintptr_t)objs;
	wait_args.count = 2;
	wait_args.owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ((uintptr_t)objs, wait_args.objs);
	EXPECT_EQ(2, wait_args.count);
	EXPECT_EQ(123, wait_args.owner);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	wait_args.owner = 456;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	wait_args.owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(0, ret);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	sem_args.count = 3;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, sem_args.count);

	owner = 123;
	ret = ioctl(fd, WINESYNC_IOC_KILL_OWNER, &owner);
	EXPECT_EQ(0, ret);

	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);

	sem_args.count = 0xdeadbeef;
	sem_args.max = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, sem_args.count);
	EXPECT_EQ(3, sem_args.max);

	mutex_args.count = 0xdeadbeef;
	mutex_args.owner = 0xdeadbeef;
	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, mutex_args.count);
	EXPECT_EQ(123, mutex_args.owner);

	/* test waiting on the same object twice */
	objs[0] = objs[1] = sem_args.sem;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_DELETE, &sem_args.sem);
	EXPECT_EQ(0, ret);
	ret = ioctl(fd, WINESYNC_IOC_DELETE, &mutex_args.mutex);
	EXPECT_EQ(0, ret);

	close(fd);
}

TEST(invalid_objects)
{
	struct winesync_mutex_args mutex_args = {0};
	struct winesync_wait_args wait_args = {0};
	struct winesync_sem_args sem_args = {0};
	__u32 objs[2] = {0};
	int fd, ret;

	fd = open("/dev/winesync", O_CLOEXEC | O_RDONLY);
	ASSERT_LE(0, fd);

	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_PUT_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	wait_args.objs = (uintptr_t)objs;
	wait_args.count = 1;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_DELETE, &objs[0]);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	sem_args.max = 1;
	ret = ioctl(fd, WINESYNC_IOC_CREATE_SEM, &sem_args);
	EXPECT_EQ(0, ret);

	mutex_args.mutex = sem_args.sem;
	ret = ioctl(fd, WINESYNC_IOC_PUT_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_READ_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	objs[0] = sem_args.sem;
	objs[1] = sem_args.sem + 1;
	wait_args.count = 2;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	objs[0] = sem_args.sem + 1;
	objs[1] = sem_args.sem;
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ANY, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);
	ret = ioctl(fd, WINESYNC_IOC_WAIT_ALL, &wait_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_DELETE, &sem_args.sem);
	EXPECT_EQ(0, ret);

	ret = ioctl(fd, WINESYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);

	sem_args.sem = mutex_args.mutex;
	ret = ioctl(fd, WINESYNC_IOC_PUT_SEM, &sem_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_READ_SEM, &sem_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = ioctl(fd, WINESYNC_IOC_DELETE, &mutex_args.mutex);
	EXPECT_EQ(0, ret);

	close(fd);
}

TEST_HARNESS_MAIN
