#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
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

/* Dear reader, this solution COULD cause lead to some nasty
   situations if there are more than one breakable pipe per
   process. Now I use fork() so I'm rather safe, because every
   named pipe is handled in separate file. If I'd switch to
   threads it could lead to some nasty conditions. :-D */
bool pipbrk = false;
static void brk_pipe_handler(int signal) {
	pipbrk = true;
}

typedef struct {
	bool read_break, write_break;
} ForwardResult;
 
static ForwardResult forward_stream(int in, int out) {
	static char buff[0x100];
	static size_t buff_fill = 0;
	size_t in_size;

	pipbrk = false;

	if(buff_fill > 0)
		write(out, buff, buff_fill);

	if (pipbrk) {
		pipbrk = false;
		return (ForwardResult){.write_break = true};
	}

	while((in_size = read(in, buff, 0x100))>0) {
		if (pipbrk) {
			pipbrk = false;
			return (ForwardResult){.read_break = true};
		}

		write(out, buff, in_size);
		if(pipbrk) {
			buff_fill = in_size;
			pipbrk = false;
			return (ForwardResult){.write_break = true};
		}
	}

	return (ForwardResult){.read_break = false, .write_break = false};
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

typedef void (*StreamAct)(int, void*);
typedef struct {
	StreamAct in, out, err;
	void *in_ctx, *out_ctx, *err_ctx;
} StreamActions;

static void handle_streams(StreamSet streams, StreamActions actions) {
	StreamAct acts[3] = {actions.out, actions.err, actions.in};
	void      *ctxs[3] = {actions.out_ctx, actions.err_ctx, actions.in_ctx};
	int	  fds[3] = {streams.out, streams.err, streams.in};
	pid_t	  pids[3];

	for(uint i = 0; i < 3; i++) {
		pids[i] = fork();
		if(pids[i] < 0)
			die("fork failed")
		else if(pids[i] == 0) {
			for(uint j = 0; j < 3; j++)
				if(j!=i) close(fds[j]);

			acts[i](fds[i], ctxs[i]);
			exit(0);
		}
	}
}

static void execute(void *_args) {
	char **args = (char**) _args;

	execvp(args[0], args);
	fprintf(stderr, "box: cannot run %s\n", args[0]);
	exit(1);
}

static void handle_input(int fd, void *ctx){
	while(1) {
		int input = open((char*)ctx, O_RDONLY);
		if(input <= 0)
			die("open " input_fifo);

		if(forward_stream(input, fd).write_break){
			fprintf(stderr, "box/input: write break\n");
			exit(1);
		}

		close(input);
	}
}

static void handle_output(int fd, void *ctx) {
	while(1) {
		int output = open((char*)ctx, O_WRONLY);
		if(output <= 0)
			die("open " output_fifo);

		if(forward_stream(fd, output).read_break){
			fprintf(stderr, "box/output: read break\n");
			exit(1);
		}

		close(output);
	}
}

#define Signal_Handler(Sig, Handler) { \
	struct sigaction __act = (struct sigaction) { \
		.sa_handler = Handler \
	}; \
	sigaction(Sig, &__act, 0); \
}

int main(int argc, char **argv){
	Signal_Handler(SIGINT, &cleanup);
	Signal_Handler(SIGPIPE, &brk_pipe_handler);

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
	handle_streams(program_streams, (StreamActions){
		.in = &handle_input, .in_ctx = input_fifo,
		.out = &handle_output, .out_ctx = output_fifo,
		.err = &handle_output, .err_ctx = error_fifo
	});

	return 0;
}