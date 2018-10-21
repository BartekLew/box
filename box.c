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

static void cleanup(int signal) {
	unlink(input_fifo);
	unlink(output_fifo);
	exit(0);
}

static void forward_stream(int in, int out) {
	char buff[0x100];
	size_t in_size;

	while((in_size = read(in, buff, 0x100))>0)
		write(out, buff, in_size);
}

typedef struct {
	int in,out;
} StreamSet;

static StreamSet meanwhile(void (*f)(void*), void *ctx) {
	int input_pipe[2];
	if(pipe(input_pipe) != 0)
		die("pipe");

	int output_pipe[2];
	if(pipe(output_pipe) != 0)
		die("pipe");

	pid_t sbt_pid = fork();
	if(sbt_pid < 0)
		die("fork")

	else if(sbt_pid == 0) {
		close(input_pipe[1]);
		close(output_pipe[0]);
		dup2(input_pipe[0], STDIN_FILENO);
		dup2(output_pipe[1], STDOUT_FILENO);

		f(ctx);
		exit(0);
	}

	close(input_pipe[0]);
	close(output_pipe[1]);

	return (StreamSet) {
		.in = input_pipe[1],
		.out = output_pipe[0]
	};
}

typedef void (*StreamAct)(int, void*);
typedef struct {
	StreamAct in, out;
	void *in_ctx, *out_ctx;
} StreamActions;

static void handle_streams(StreamSet streams, StreamActions actions) {
	pid_t writer_pid = fork();
	if(writer_pid < 0)
		die("fork")
	else if(writer_pid == 0) {
		close(streams.in);
		actions.out(streams.out, actions.out_ctx);
		exit(0);
	}

	close(streams.out);
	actions.in(streams.in, actions.in_ctx);
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

		forward_stream(input, fd);

		close(input);
	}
}

static void handle_output(int fd, void *ctx) {
	while(1) {
		int output = open((char*)ctx, O_WRONLY);
		if(output <= 0)
			die("open " output_fifo);

		forward_stream(fd, output);

		close(output);
	}
}

int main(int argc, char **argv){
	struct sigaction act = (struct sigaction) {
		.sa_handler = &cleanup
	};
	sigaction(SIGINT, &act,0);

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

	if(mkfifo(input_fifo, 0660)!=0)
		die("mkfifo " input_fifo);
	if(mkfifo(output_fifo, 0660)!=0)
		die("mkfifo " output_fifo);

	StreamSet program_streams = meanwhile(&execute, (void*)args);
	handle_streams(program_streams, (StreamActions){
		.in = &handle_input, .in_ctx = input_fifo,
		.out = &handle_output, .out_ctx = output_fifo
	});

	return 0;
}
