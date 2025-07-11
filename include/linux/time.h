#ifndef _LINUX_TIME_H
#define _LINUX_TIME_H

#include <linux/types.h>

#ifdef __KERNEL__
# include <linux/seqlock.h>
#endif

#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec {
	time_t	tv_sec;		/* seconds */
	long	tv_nsec;	/* nanoseconds */
};
#endif

struct timeval {
	time_t		tv_sec;		/* seconds */
	suseconds_t	tv_usec;	/* microseconds */
};

struct timezone {
	int	tz_minuteswest;	/* minutes west of Greenwich */
	int	tz_dsttime;	/* type of dst correction */
};

#ifdef __KERNEL__

/* Parameters used to convert the timespec values: */
#define MSEC_PER_SEC		1000L
#define USEC_PER_SEC		1000000L
#define NSEC_PER_SEC		1000000000L
#define NSEC_PER_USEC		1000L

static __inline__ int timespec_equal(struct timespec *a, struct timespec *b)
{
	return (a->tv_sec == b->tv_sec) && (a->tv_nsec == b->tv_nsec);
}

extern unsigned long mktime(const unsigned int year, const unsigned int mon,
			    const unsigned int day, const unsigned int hour,
			    const unsigned int min, const unsigned int sec);

extern void set_normalized_timespec(struct timespec *ts, time_t sec, long nsec);

/*
 * Returns true if the timespec is norm, false if denorm:
 */
#define timespec_valid(ts) \
	(((ts)->tv_sec >= 0) && (((unsigned long) (ts)->tv_nsec) < NSEC_PER_SEC))

/*
 * 64-bit nanosec type. Large enough to span 292+ years in nanosecond
 * resolution. Ought to be enough for a while.
 */
typedef s64 nsec_t;

extern struct timespec xtime;
extern struct timespec wall_to_monotonic;
extern seqlock_t xtime_lock;

static inline unsigned long get_seconds(void)
{
	return xtime.tv_sec;
}

struct timespec current_kernel_time(void);

#define CURRENT_TIME		(current_kernel_time())
#define CURRENT_TIME_SEC	((struct timespec) { xtime.tv_sec, 0 })

extern void do_gettimeofday(struct timeval *tv);
extern int do_settimeofday(struct timespec *tv);
extern int do_sys_settimeofday(struct timespec *tv, struct timezone *tz);
#define do_posix_clock_monotonic_gettime(ts) ktime_get_ts(ts)
extern long do_utimes(int dfd, char __user *filename, struct timeval *times);
struct itimerval;
extern int do_setitimer(int which, struct itimerval *value,
			struct itimerval *ovalue);
extern int do_getitimer(int which, struct itimerval *value);
extern void getnstimeofday(struct timespec *tv);

extern struct timespec timespec_trunc(struct timespec t, unsigned gran);

/**
 * timespec_to_ns - Convert timespec to nanoseconds
 * @ts:		pointer to the timespec variable to be converted
 *
 * Returns the scalar nanosecond representation of the timespec
 * parameter.
 */
static inline nsec_t timespec_to_ns(const struct timespec *ts)
{
	return ((nsec_t) ts->tv_sec * NSEC_PER_SEC) + ts->tv_nsec;
}

/**
 * timeval_to_ns - Convert timeval to nanoseconds
 * @ts:		pointer to the timeval variable to be converted
 *
 * Returns the scalar nanosecond representation of the timeval
 * parameter.
 */
static inline nsec_t timeval_to_ns(const struct timeval *tv)
{
	return ((nsec_t) tv->tv_sec * NSEC_PER_SEC) +
		tv->tv_usec * NSEC_PER_USEC;
}

/**
 * ns_to_timespec - Convert nanoseconds to timespec
 * @nsec:	the nanoseconds value to be converted
 *
 * Returns the timespec representation of the nsec parameter.
 */
extern struct timespec ns_to_timespec(const nsec_t nsec);

/**
 * ns_to_timeval - Convert nanoseconds to timeval
 * @nsec:	the nanoseconds value to be converted
 *
 * Returns the timeval representation of the nsec parameter.
 */
extern struct timeval ns_to_timeval(const nsec_t nsec);

#endif /* __KERNEL__ */

#define NFDBITS			__NFDBITS

#define FD_SETSIZE		__FD_SETSIZE
#define FD_SET(fd,fdsetp)	__FD_SET(fd,fdsetp)
#define FD_CLR(fd,fdsetp)	__FD_CLR(fd,fdsetp)
#define FD_ISSET(fd,fdsetp)	__FD_ISSET(fd,fdsetp)
#define FD_ZERO(fdsetp)		__FD_ZERO(fdsetp)

/*
 * Names of the interval timers, and structure
 * defining a timer setting:
 */
#define	ITIMER_REAL		0
#define	ITIMER_VIRTUAL		1
#define	ITIMER_PROF		2

struct itimerspec {
	struct timespec it_interval;	/* timer period */
	struct timespec it_value;	/* timer expiration */
};

struct itimerval {
	struct timeval it_interval;	/* timer interval */
	struct timeval it_value;	/* current value */
};

/*
 * The IDs of the various system clocks (for POSIX.1b interval timers):
 */
#define CLOCK_REALTIME			0
#define CLOCK_MONOTONIC			1
#define CLOCK_PROCESS_CPUTIME_ID	2
#define CLOCK_THREAD_CPUTIME_ID		3

/*
 * The IDs of various hardware clocks:
 */
#define CLOCK_SGI_CYCLE			10
#define MAX_CLOCKS			16
#define CLOCKS_MASK			(CLOCK_REALTIME | CLOCK_MONOTONIC)
#define CLOCKS_MONO			CLOCK_MONOTONIC

/*
 * The various flags for setting POSIX.1b interval timers:
 */
#define TIMER_ABSTIME			0x01

#endif
