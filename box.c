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

void execute(void *_args) {
	char **args = (char**) _args;

	execvp(args[0], args);
	fprintf(stderr, "box: cannot run %s\n", args[0]);
	exit(1);
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

	pid_t writer_pid = fork();
	if(writer_pid < 0)
		die("fork")
	else if(writer_pid == 0) {
		close(program_streams.in);

		while(1) {
			int output = open(output_fifo, O_WRONLY);
			if(output <= 0)
				die("open " output_fifo);

			forward_stream(program_streams.out, output);

			close(output);
		}
		exit(1);
	}

	close(program_streams.out);
	while(1) {
		int input = open(input_fifo, O_RDONLY);
		if(input <= 0)
			die("open " input_fifo);

		forward_stream(input, program_streams.in);

		close(input);
	}

	return 0;
}
