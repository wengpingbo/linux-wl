#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MSEC_PER_SEC	1000L
#define USEC_PER_MSEC	1000L
#define MAX_THREADS	20

struct thread_data {
	unsigned int period, exec; /* in ms */

	int cpu;
	int policy;
	int num;
};

struct wl_params {
	unsigned int duration; /* in sec */
	unsigned int thread_num;
	struct thread_data *threads[MAX_THREADS];
};

struct timespec msec_to_timespec(unsigned int msec)
{
	struct timespec ts;

	ts.tv_sec = msec / 1000;
	ts.tv_nsec = (msec % 1000) * 1000000;

	return ts;
}

struct timespec timespec_add(struct timespec *t1, struct timespec *t2)
{
	struct timespec ts;

	ts.tv_sec = t1->tv_sec + t2->tv_sec;
	ts.tv_nsec = t1->tv_nsec + t2->tv_nsec;

	while (ts.tv_nsec >= 1E9) {
		ts.tv_nsec -= 1E9;
		ts.tv_sec++;
	}

	return ts;
}

struct timespec timespec_sub(struct timespec *t1, struct timespec *t2)
{
	struct timespec ts;

	if (t1->tv_nsec < t2->tv_nsec) {
		ts.tv_sec = t1->tv_sec - t2->tv_sec -1;
		ts.tv_nsec = t1->tv_nsec  + 1000000000 - t2->tv_nsec;
	} else {
		ts.tv_sec = t1->tv_sec - t2->tv_sec;
		ts.tv_nsec = t1->tv_nsec - t2->tv_nsec;
	}

	return ts;

}

int timespec_after(struct timespec *t1, struct timespec *t2)
{
	if(t1->tv_sec > t2->tv_sec)
		return 1;

	if(t1->tv_sec == t2->tv_sec && t1->tv_nsec > t2->tv_nsec)
		return 1;

	return 0;
}

void usage(void)
{
	printf("./wl [-d duration] [-h] [-t p:e:c:s:n] [-t p:e:c:s:n]\n");
}

void run(struct thread_data *t)
{
	struct timespec t1, t2, now;

	usleep((t->period - t->exec) * USEC_PER_MSEC);

	t2 = msec_to_timespec(t->exec);
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
	t2 = timespec_add(&t1, &t2);
	do {
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	} while(!timespec_after(&now, &t2));
}

int sched_handle_policy(char *polname)
{
	int policy;

	if (strncasecmp(polname, "other", 5) == 0)
		policy = SCHED_OTHER;
	else if (strncasecmp(polname, "batch", 5) == 0)
		policy = SCHED_BATCH;
	else if (strncasecmp(polname, "idle", 4) == 0)
		policy = SCHED_IDLE;
	else if (strncasecmp(polname, "fifo", 4) == 0)
		policy = SCHED_FIFO;
	else if (strncasecmp(polname, "rr", 2) == 0)
		policy = SCHED_RR;
	else	/* default policy if we don't recognize the request */
		policy = SCHED_OTHER;

	return policy;
}

/*
 * Parse str period:execute:affinity:sched_class:num
 * Fill with '-', if one of field is empty
 *
 * @period: thread period time, in ms
 * @period: thread execute time, must smaller than period, in ms
 * @affinity: single cpu affinity num
 * @sched_class: sched class str, must be one of "other, batch, idle, fifo, rr"
 * @num: thread num with same settings
 */
struct thread_data *parse_thread_data(char *str)
{
	struct thread_data *opts = (struct thread_data *)malloc(sizeof(struct thread_data));
	char *opt = strtok(str, ":");
	int i = 0;

	memset(opts, 0, sizeof(struct thread_data));
	opts->cpu = opts->policy = -1;
	opts->num = 1;

	while (opt) {
		switch (i) {
		case 0:
			opts->period = atoi(opt);
			break;
		case 1:
			opts->exec = atoi(opt);
			break;
		case 2:
			if (opt[0] != '-')
				opts->cpu = atoi(opt);
			break;
		case 3:
			if (opt[0] != '-')
				opts->policy = sched_handle_policy(opt);
			break;
		case 4:
			if (opt[0] != '-')
				opts->num = atoi(opt);
			break;
		default:
			fprintf(stderr, "Invalid option %s\n", str);
			goto err;
		}

		opt = strtok(NULL, ":");
		i++;
	}

	return opts;
err:
	free(opts);
	return NULL;
}

int parse_args(int argc, char* argv[], struct wl_params *wl)
{
	int c, cnt = 0;
	struct thread_data *thread;

	while((c = getopt(argc, argv, "d:t:h")) != -1) {
		if (cnt >= MAX_THREADS) {
			fprintf(stderr, "reach the max threads %d, ignore rest\n", MAX_THREADS);
			break;
		}

		switch (c) {
		case 'd':
			wl->duration = atoi(optarg);
			break;
		case 't':
			thread = parse_thread_data(optarg);
			if (thread)
				wl->threads[cnt++] = thread;
			break;
		case 'h':
		default:
			usage();
			exit(0);
		}
	}

	wl->thread_num = cnt;

	return 0;
}

void thread_run(struct thread_data *t, int loop)
{
	if (t->cpu > 0) {
		cpu_set_t cpus;

		CPU_ZERO(&cpus);
		CPU_SET(t->cpu, &cpus);
		if (sched_setaffinity(0, sizeof(cpu_set_t), &cpus))
			fprintf(stderr, "set cpu affinity failed, cpu %d\n", t->cpu);
	}

	if (t->policy > 0) {
		struct sched_param s_param;

		memset(&s_param, 0, sizeof(s_param));
		s_param.sched_priority = sched_get_priority_max(t->policy);
		if (sched_setscheduler(0, t->policy, &s_param))
			fprintf(stderr, "set scheduler failed, policy %d\n", t->policy);
	}

	while (loop--)
		run(t);
}

int launch_thread(struct thread_data *t, int loop)
{
	int ret = 0, i = 0;

	for (; i < t->num; i++)
	{
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			exit(EXIT_FAILURE);
		}

		if (pid == 0) {
			thread_run(t, loop);
			exit(EXIT_SUCCESS);
		}
	}

	return i;
}

void clean_stuffs(struct wl_params *wl)
{
	int i = 0;

	for (; i < wl->thread_num; i++)
		free(wl->threads[i]);
}

int main(int argc, char* argv[])
{
	struct wl_params wl;
	int status, i;

	memset(&wl, 0, sizeof(wl));
	parse_args(argc, argv, &wl);

	for (i = 0; i < wl.thread_num; i++)
	{
		struct thread_data *t = wl.threads[i];
		int loop = wl.duration * MSEC_PER_SEC / t->period;

		printf("period %ums, exec %ums, loop %d, cpu %d, policy %d, num %d\n",
				t->period, t->exec, loop, t->cpu, t->policy, t->num);
		launch_thread(t, loop);
	}

	/* wait for all children to exit */
	while(wait(&status) > 0);

	clean_stuffs(&wl);

	return 0;
}
