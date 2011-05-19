/*
 *  Yet another V4L2 video capture example
 *  
 *  Copyright Scott Ellis, 2011 (C)
 *
 *  This program can be used and distributed without restrictions.
 *
 *  Modified and hard-coded for my own camera testing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h> 
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <asm/types.h>
#include <signal.h>
#include <linux/videodev2.h>
#include <pthread.h>

#define V4L2_MT9P031_GREEN1_GAIN		(V4L2_CID_PRIVATE_BASE + 0)
#define V4L2_MT9P031_BLUE_GAIN			(V4L2_CID_PRIVATE_BASE + 1)
#define V4L2_MT9P031_RED_GAIN			(V4L2_CID_PRIVATE_BASE + 2)
#define V4L2_MT9P031_GREEN2_GAIN		(V4L2_CID_PRIVATE_BASE + 3)

#define MAX_MMAP_BUFFERS 8

struct mmap_buffer {
        void *start;
        size_t length;
		unsigned int count;
};

#define GREEN1_GAIN 0
#define BLUE_GAIN 1
#define RED_GAIN 2
#define GREEN2_GAIN 3
#define GLOBAL_GAIN 4

int gain[5];

int fd;
int exposure_us;
int pixel_format;
int image_width ;
int image_height;
int no_snap;
struct mmap_buffer mm_buff[MAX_MMAP_BUFFERS];
unsigned int num_buffers;
int streaming;
int shutdown_time;
int buffer_index_to_save;

pthread_mutex_t img_proc_mutex;
pthread_cond_t	img_proc_cv;

static int msleep(int milliseconds)
{
	struct timespec ts;

	if (milliseconds < 1)
		return -2;

	ts.tv_sec = milliseconds / 1000;
	ts.tv_nsec = 1000000 * (milliseconds % 1000);

	return nanosleep(&ts, NULL);
}

static int xioctl(int fd, int request, void *arg)
{
	int r;

	do {
		r = ioctl(fd, request, arg);
	} while (-1 == r && EINTR == errno);

	return r;
}

static int set_exposure(int fd, int exposure)
{
	struct v4l2_control control;

	memset(&control, 0, sizeof (control));
	control.id = V4L2_CID_EXPOSURE;
	control.value = exposure;

	if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control)) {
	        perror("VIDIOC_S_CTRL");
		return -1;
	}

	return 0;
}

static int set_gain(int fd, int control_id, int gain)
{
	struct v4l2_control control;

	memset(&control, 0, sizeof (control));
	control.id = control_id;
	control.value = gain;

	if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control)) {
	        perror("VIDIOC_S_CTRL");
		return -1;
	}

	return 0;
}

static void set_controls(void)
{
	if (exposure_us > 0)
		set_exposure(fd, exposure_us);

	if (gain[GLOBAL_GAIN] > 0) {
		set_gain(fd, V4L2_CID_GAIN, gain[GLOBAL_GAIN]);
	}
	else {
		if (gain[GREEN1_GAIN] > 0)
			set_gain(fd, V4L2_MT9P031_GREEN1_GAIN, gain[GREEN1_GAIN]);

		if (gain[RED_GAIN] > 0)
			set_gain(fd, V4L2_MT9P031_RED_GAIN, gain[RED_GAIN]);

		if (gain[BLUE_GAIN] > 0)
			set_gain(fd, V4L2_MT9P031_BLUE_GAIN, gain[BLUE_GAIN]);

		if (gain[GREEN2_GAIN] > 0)
			set_gain(fd, V4L2_MT9P031_GREEN2_GAIN, gain[GREEN2_GAIN]);
	}
}

static void write_image(unsigned int buff_index)
{
	int image_fd;
	char file[64];
	int flags = O_CREAT | O_RDWR | O_TRUNC;
	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;

	if (buff_index >= num_buffers)
		return;

	if (pixel_format == V4L2_PIX_FMT_SGRBG10)
		sprintf(file, "bayer_%d.img", mm_buff[buff_index].count);
	else if (pixel_format == V4L2_PIX_FMT_YUYV)
		sprintf(file, "yuyv_%d.img", mm_buff[buff_index].count);
	else 
		sprintf(file, "uyvy_%d.img", mm_buff[buff_index].count);


	image_fd = open(file, flags, mode);

	if (image_fd < 0) {
		perror("open(<image>)");
		return;
	}

	write(image_fd, (void *)mm_buff[buff_index].start, mm_buff[buff_index].length);

	close(image_fd);
}

static int stream_off(void)
{
	enum v4l2_buf_type type;

	if (streaming) {
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type)) {
			perror("VIDIOC_STREAMOFF");
			return -1;
		}

		streaming = 0;
	}

	return 0;
}

static int queue_buffer(unsigned int index)
{
	struct v4l2_buffer buf;

	if (index >= num_buffers)
		return -1;

	memset(&buf, 0, sizeof(buf));

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = index;

	if (-1 == xioctl (fd, VIDIOC_QBUF, &buf)) {
		perror("VIDIOC_QBUF");
		return -1;
	}

	return 0;
}

static int stream_on(void)
{
	unsigned int i;
	enum v4l2_buf_type type;

	if (streaming) {
		printf("Already streaming\n");
		return -1;
	}

	// queue all buffers
	for (i = 0; i < num_buffers; i++) {
		if (queue_buffer(i) < 0)
			return -1;
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (-1 == xioctl(fd, VIDIOC_STREAMON, &type)) {
		perror("VIDIOC_STREAMON");
		return -1;
	}

	streaming = 1;

	return 0;
}

static void unmap_mmap_buffers(void)
{
	int i;

	for (i = 0; i < num_buffers; i++) {
		if (mm_buff[i].start) {
			if (-1 == munmap(mm_buff[i].start, mm_buff[i].length)) {
				perror("munmap");
			}

			mm_buff[i].start = 0;
		}
	}
}

static int init_mmap_buffers(void)
{
	int i;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;

	memset(&req, 0, sizeof(req));

	req.count = MAX_MMAP_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		perror("VIDIOC_REQBUFS");
		return -1;
	}

	if (req.count < MAX_MMAP_BUFFERS)
		printf("VIDIOC_REQBUFS only gave us %u buffers\n", req.count);

	num_buffers = req.count;

	// get the addresses for our mmap'd buffers
	for (i = 0; i < num_buffers; i++) {
		memset(&buf, 0, sizeof(buf));

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf)) {
			perror("VIDIOC_QUERYBUF");
			return -1;
		}

		mm_buff[i].length = buf.length;
		mm_buff[i].start = mmap(NULL, 
									buf.length, 
									PROT_READ | PROT_WRITE,
									MAP_SHARED,
									fd, 
									buf.m.offset);

		if (MAP_FAILED == mm_buff[i].start) {
			perror("mmap");
			return -1;
		}
	}

	return 0;
}

static int init_image_format(void)
{
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = image_width;
	fmt.fmt.pix.height = image_height;
	fmt.fmt.pix.pixelformat = pixel_format;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt)) {
		perror("VIDIOC_S_FMT");
		return -1;
	}

	if (fmt.fmt.pix.width != image_width) {
		printf("Width changed after VIDIOC_S_FMT\n");
		printf("Requested width = %d\n", image_width);
		printf("Got fmt.fmt.pix.width = %d\n", fmt.fmt.pix.width);
		return -1;
	}

 	if (fmt.fmt.pix.height != image_height) {
		printf("Height changed after VIDIOC_S_FMT\n");
		printf("Requested height = %d\n", image_height);
		printf("Got fmt.fmt.pix.height = %d\n", fmt.fmt.pix.height);
		return -1;
	}

	if (fmt.fmt.pix.pixelformat != pixel_format) {
		printf("pixelformat changed after VIDIOC_S_FMT\n");
		return -1;
	}

	return 0;
}

/* 
   Return 1 if we got a frame and update buff_index
   Return 0 if no frame is ready
   Return -1 for errors
*/
static int read_frame(int *buff_index)
{
	struct v4l2_buffer buf;

	memset(&buf, 0, sizeof(buf));

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
		switch (errno) {
		case EAGAIN:
			// no buffer ready
			return 0;

		case EIO:
			/* Could ignore EIO, see spec. */
			/* fall through */

		default:
			perror("VIDIOC_DQBUF");
			return -1;
		}
	}

	if (buf.index >= num_buffers) {
		printf("VIDIOC_DQBUF returned invalid buf.index: %u\n", buf.index);
		return -1;
	}

	mm_buff[buf.index].count++;

	if (buff_index) {
		*buff_index = buf.index;
	}
	else {
		if (queue_buffer(buf.index) < 0)
			return -1;
	}

	return 1;
}

static void dump_stats(void)
{
	int i, total;

	total = 0;

	printf("Stats: buffer_index: %d  ", buffer_index_to_save);

	for (i = 0; i < num_buffers; i++) {
		printf("buff[%d]: %d  ", i, mm_buff[i].count);
		total += mm_buff[i].count;
	}

	printf("Total: %d\n", total);
}

void *thread_proc(void *args)
{
	pthread_mutex_lock(&img_proc_mutex);

	while (!shutdown_time) {
		pthread_cond_wait(&img_proc_cv, &img_proc_mutex);

		if (buffer_index_to_save < 0)
			break;

		// fake some work
		dump_stats();
		write_image(buffer_index_to_save);

		// requeue the buffer, this might need some mutex protection
		queue_buffer(buffer_index_to_save);

		buffer_index_to_save = -1;
	}

	pthread_mutex_unlock(&img_proc_mutex);
	pthread_exit(0);
}

static void imaging_loop(void)
{
	fd_set fds;
	struct timeval tv;
	int i, r, buff_index;
	pthread_t img_proc_thread;

	pthread_mutex_init(&img_proc_mutex, NULL);
	pthread_cond_init(&img_proc_cv, NULL);

	if (pthread_create(&img_proc_thread, NULL, thread_proc, NULL)) {
		perror("pthread_create");
		return;
	}

	buffer_index_to_save = -1;

	i = 0;

	while (!shutdown_time) {
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		tv.tv_sec = 30;
		tv.tv_usec = 0;

		r = select(fd + 1, &fds, NULL, NULL, &tv);

		if (-1 == r) {
			if (EINTR == errno) {
				continue;
			}
			else {
				perror("select");
				break;
			}
		}

		if (0 == r) {
			fprintf(stderr, "select timeout\n");
			break;
		}

		r = read_frame(&buff_index);

		if (r < 0)
			break;

		if (r == 0)
			continue;


		if (++i > 100) {
			if (!pthread_mutex_trylock(&img_proc_mutex)) {
				if (buffer_index_to_save < 0) {
					buffer_index_to_save = buff_index;
					pthread_cond_signal(&img_proc_cv);
					pthread_mutex_unlock(&img_proc_mutex);					
				}
				else {
					pthread_mutex_unlock(&img_proc_mutex);

					// not going to use this buffer so requeue
					if (queue_buffer(buff_index) < 0)
						break;
				}
			}

			i = 0;
		}
		else {
			// requeue the buffer
			if (queue_buffer(buff_index) < 0)
				break;
		}
	}

	if (img_proc_thread) {
		shutdown_time = 1;
		if (!pthread_mutex_lock(&img_proc_mutex)) {
			pthread_cond_signal(&img_proc_cv);
			pthread_mutex_unlock(&img_proc_mutex);
			pthread_join(img_proc_thread, NULL);
		}
	}
}

static void sig_handler(int sig)
{
	if (sig == SIGINT)
		shutdown_time = 1;
}
 
static void install_signal_handlers()
{
	struct sigaction sia;

	bzero(&sia, sizeof sia);
	sia.sa_handler = sig_handler;

	if (sigaction(SIGINT, &sia, NULL) < 0) {
		perror("sigaction(SIGINT)");
		exit(1);
	} 
}

static void usage(char *argv_0)
{
	printf("Usage: %s [options]\n\n"
			"Options:\n"
			"-f | --format        Pixel format [uyvy, yuyv, bayer] (default uyvy)\n"
			"-s | --size          Image size  0:2560x1920  1:1280x960  2:640x480\n"
			"-e | --exposure      Exposure time in microseconds\n"
			"-r | --red           Red gain\n"
			"-b | --blue          Blue gain\n"
			"-G | --green1        Green1 gain\n"
			"-g | --green2        Green2 gain\n"
			"-n | --gain          Global gain\n"
			"-o | --nosnap        Only apply gain/exposure settings, no picture\n"
			"-h | --help          Print this message\n"
			"",
			argv_0);
}

static const char short_options [] = "f:s:e:r:b:G:g:n:oh";

static const struct option long_options [] = {
	{ "format",	required_argument,	NULL,	'f' },
	{ "size",	required_argument,	NULL,	's' },
	{ "exposure",	required_argument,	NULL,	'e' },
	{ "red",	required_argument,	NULL,	'r' },
	{ "blue",	required_argument,	NULL,	'b' },
	{ "green1",	required_argument,	NULL,	'G' },
	{ "green2",	required_argument,	NULL,	'g' },
	{ "gain",	required_argument,	NULL,	'n' },
	{ "nosnap",	no_argument,		NULL,	'o' },
	{ "help",	no_argument,		NULL,	'h' },
	{ 0, 0, 0, 0 }
};

void get_parameters(int argc, char **argv)
{
	int index;
	int c, size;

	size = 0;
	pixel_format = V4L2_PIX_FMT_UYVY;
	no_snap = 0;
	image_width = 2560;
	image_height = 1920;

	for (;;) {
		c = getopt_long(argc, argv, short_options, long_options, &index);

		if (-1 == c)
			break;

		switch (c) {
		case 0: /* getopt_long() flag */
			break;

		case 's':
			size = atoi(optarg);

			if (size < 0 || size > 2) {
				printf("Invalid size parameter %d\n", size);
				exit(EXIT_FAILURE);
			}
				
			break;

		case 'e':
			exposure_us = atol(optarg);

			if (exposure_us < 63)
				exposure_us = 63;
			else if (exposure_us > 142644)
				exposure_us = 142644;

			break;

		case 'r':
			gain[RED_GAIN] = atol(optarg);

			if (gain[RED_GAIN] < 1 || gain[RED_GAIN] > 161)
				gain[RED_GAIN] = 0;

			break;

		case 'b':
			gain[BLUE_GAIN] = atol(optarg);

			if (gain[BLUE_GAIN] < 1 || gain[BLUE_GAIN] > 161)
				gain[BLUE_GAIN] = 0;

			break;

		case 'G':
			gain[GREEN1_GAIN] = atol(optarg);

			if (gain[GREEN1_GAIN] < 1 || gain[GREEN1_GAIN] > 161)
				gain[GREEN1_GAIN] = 0;

			break;

		case 'g':
			gain[GREEN2_GAIN] = atol(optarg);

			if (gain[GREEN2_GAIN] < 1 || gain[GREEN2_GAIN] > 161)
				gain[GREEN2_GAIN] = 0;

			break;

		case 'n':
			gain[GLOBAL_GAIN] = atol(optarg);

			if (gain[GLOBAL_GAIN] < 1 || gain[GLOBAL_GAIN] > 161)
				gain[GLOBAL_GAIN] = 0;

			break;

		case 'f':
			if (!strcasecmp(optarg, "bayer")) {
				pixel_format = V4L2_PIX_FMT_SGRBG10;
			}
			else if (!strcasecmp(optarg, "yuyv")) {
				pixel_format = V4L2_PIX_FMT_YUYV;
			}
			else if (!strcasecmp(optarg, "uyvy")) {
				pixel_format = V4L2_PIX_FMT_UYVY;
			}
			else {
				printf("Invalid pixel format: %s\n", optarg);
				exit(EXIT_FAILURE);
			}
			
			break;
	
		case 'o':
			no_snap = 1;
			break;

		case 'h':
			usage(argv[0]);
			exit (EXIT_SUCCESS);

		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (size && pixel_format == V4L2_PIX_FMT_SGRBG10) {
		printf("Bayer format restricted to size 2560x1920\n");
		exit(EXIT_FAILURE);
	}

	if (size == 2) {
		image_width = 640;
		image_height = 480;
	}
	else if (size == 1) {
		image_width = 1280;
		image_height = 960;
	}
	else {
		image_width = 2560;
		image_height = 1920;
	}
}

int main(int argc, char **argv)
{
	get_parameters(argc, argv);

	install_signal_handlers();

	// open NON_BLOCKING so VIDIOC_DQBUF reads return immediately
	fd = open("/dev/video0", O_RDWR | O_NONBLOCK, 0);
	if (fd < 0) {
		perror("open(/dev/video0)");
		exit(EXIT_FAILURE);
	}

	if (init_image_format() < 0)
		goto done;

	if (init_mmap_buffers() < 0)
		goto done;

	set_controls();

	if (no_snap)
		goto done;

	if (stream_on() < 0)
		goto done;

	imaging_loop();

	dump_stats();

done:

	stream_off();

	unmap_mmap_buffers();

	close(fd);

	pthread_exit(NULL);
}

