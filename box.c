#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define die(MSG) { \
	fprintf(stderr, "box: " MSG "\n"); \
	exit(1); \
}

#define input_fifo "box.in"
#define output_fifo "box.out"
#define error_fifo "box.err"

static void cleanup(int signal) {
	unlink(input_fifo);
	unlink(output_fifo);
	unlink(error_fifo);
	exit(0);
}

typedef struct {
	int in,out, err;
} StreamSet;

static StreamSet meanwhile(void (*f)(void*), void *ctx) {
	int input_pipe[2];
	if(pipe(input_pipe) != 0)
		die("pipe");

	int output_pipe[2];
	if(pipe(output_pipe) != 0)
		die("pipe");

	int error_pipe[2];
	if(pipe(error_pipe) != 0)
		die("pipe");

	pid_t prog_pid = fork();
	if(prog_pid < 0)
		die("fork")

	else if(prog_pid == 0) {
		close(input_pipe[1]);
		close(output_pipe[0]);
		close(error_pipe[0]);
		dup2(input_pipe[0], STDIN_FILENO);
		dup2(output_pipe[1], STDOUT_FILENO);
		dup2(error_pipe[1], STDERR_FILENO);

		f(ctx);
		exit(0);
	}

	close(input_pipe[0]);
	close(output_pipe[1]);
	close(error_pipe[1]);

	return (StreamSet) {
		.in = input_pipe[1],
		.out = output_pipe[0],
		.err = error_pipe[0]
	};
}

typedef void (*StreamAct)(int, void*, int pids[]);
typedef struct {
	char *in, *out, *err;
} StreamFiles;

static void handle_streams(StreamSet streams, StreamFiles files) {
	if(fork() != 0) return;

	static char buff[0xfff];
	struct pollfd poll_data[6] = {
		{ .fd = open(files.in, O_RDWR, 0),	.events = POLLIN },
		{ .fd = streams.out,			.events = POLLIN },
		{ .fd = streams.err,			.events = POLLIN },

		{ .fd = streams.in,			.events = POLLOUT },
		{ .fd = open(files.out, O_RDWR, 0),	.events = POLLOUT },
		{ .fd = open(files.err, O_RDWR, 0),	.events = POLLOUT }

		// I use O_RDWR to avoid blocking on opening named pipe
		// Poll will let to read/write to them until buffer is full
	};

	for(uint i = 0; i < 6; i++)
		if(poll_data[i].fd < 0) {
			fprintf(stderr, "box: cannot open stream %d.\n", i);
			exit(1);
		}

	while(poll(poll_data, 6, 0) > 0) {
		for (uint i = 0; i < 3; i++) {
			if(poll_data[i].revents & POLLIN && poll_data[i+3].revents & POLLOUT) {
				int bytes = read(poll_data[i].fd, buff, 0xfff);
				write(poll_data[i+3].fd, buff, bytes);
			}
		}
	}

	perror("box/poll");
	fprintf(stderr, "box: terminated");
	
	for(uint i = 0; i < 6; i++) {
		close(poll_data[i].fd);
	}
}

static void execute(void *_args) {
	char **args = (char**) _args;

	execvp(args[0], args);
	fprintf(stderr, "box: cannot run %s\n", args[0]);
	exit(1);
}

#define Signal_Handler(Sig, Handler) { \
	struct sigaction __act = (struct sigaction) { \
		.sa_handler = Handler \
	}; \
	sigaction(Sig, &__act, 0); \
}

int main(int argc, char **argv){
	Signal_Handler(SIGINT, &cleanup);
	Signal_Handler(SIGTERM, &cleanup);
	Signal_Handler(SIGKILL, &cleanup);

	if(argc < 2) {
		fprintf(stderr,
			"box: run program with stdI/O through FIFO\n"
			"  USAGE: box executable [args...]\n"
		);
		exit(1);
	}

	char *args[argc];
	for(uint i = 0; i < argc-1; i++) args[i] = argv[i+1];
	args[argc-1] = NULL;

	// Intentionally skip testing result so that I can have existing file to be
	// used as input, output or error.
	mkfifo(input_fifo, 0660);
	mkfifo(output_fifo, 0660);
	mkfifo(error_fifo, 0660);

	StreamSet program_streams = meanwhile(&execute, (void*)args);
	handle_streams(program_streams, (StreamFiles){
		.in = input_fifo, .out = output_fifo, .err = error_fifo
	});

	return 0;
}