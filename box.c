#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define NAME_MAX 0xf0

char ctl_fifo[NAME_MAX]= "box.ctl";
char input_fifo[NAME_MAX] = "box.in";
char output_fifo[NAME_MAX] = "box.out";
char error_fifo[NAME_MAX] = "box.err";

static void cleanup(int signal) {
	unlink(ctl_fifo);
	unlink(input_fifo);
	unlink(output_fifo);
	unlink(error_fifo);
	exit(0);
}

typedef struct {
	int	in,out, err;
	pid_t	pid;
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
		.err = error_pipe[0],
		.pid = prog_pid
	};
}

typedef void (*StreamAct)(int, void*, int pids[]);
typedef struct {
	char *in, *out, *err, *ctl;
} StreamFiles;

typedef struct {
	char	*name;
	size_t	len;
	void	(*action)(StreamSet, struct pollfd *pollfd);
} CtlCmd;

void cmd_term(StreamSet streams, struct pollfd* pollfd) {
	kill(streams.pid, SIGKILL);
	cleanup(0);
	exit(0);
}

void cmd_cls(StreamSet streams, struct pollfd* pollfd) {
	ftruncate(pollfd[5].fd, 0);
	lseek(pollfd[5].fd, 0, SEEK_SET);
	ftruncate(pollfd[6].fd, 0);
	lseek(pollfd[6].fd, 0, SEEK_SET);
}

CtlCmd ctl_commands[] = {
	{ .name = "k", .len = 1, .action = &cmd_term },
	{ .name = "c", .len = 1, .action = &cmd_cls }
};

static void handle_streams(StreamSet streams, StreamFiles files) {
	if(fork() != 0) return;

	static char buff[0xfff];
	struct pollfd poll_data[7] = {
		{ .fd = open(files.ctl, O_RDWR, 0),	.events = POLLIN },

		{ .fd = open(files.in, O_RDWR, 0),	.events = POLLIN },
		{ .fd = streams.out,			.events = POLLIN },
		{ .fd = streams.err,			.events = POLLIN },

		{ .fd = streams.in,			.events = POLLOUT },
		{ .fd = open(files.out, O_RDWR, 0),	.events = POLLOUT },
		{ .fd = open(files.err, O_RDWR, 0),	.events = POLLOUT }

		// I use O_RDWR to avoid blocking on opening named pipe
		// Poll will let to read/write to them until buffer is full
	};

	for(uint i = 0; i < 7; i++)
		if(poll_data[i].fd < 0) {
			fprintf(stderr, "box: cannot open stream %d.\n", i);
			perror("box/open");
			exit(1);
		}

	while(poll(poll_data, 4, -1) > 0) {
		if(poll_data[0].revents & POLLIN) {
			int bytes = read(poll_data[0].fd, buff, 0xfff);
			for (uint i = 0; i < bytes; i++) {
				for(uint j=0; j<2; j++) {
					CtlCmd *cmd = ctl_commands+j;
					if(strncmp(buff+i, cmd->name, cmd->len) == 0) {
						cmd->action(streams, poll_data);
						i += cmd->len;
					}
				}
			}
		}

		for (uint i = 1; i < 4; i++) {
			poll_data[i+3].events = (poll_data[i].revents & POLLIN)?POLLOUT:0;
		}
		
		if(poll(poll_data+4, 3, 4000) > 0) {
			for (uint i = 1; i < 4; i++) {
				if(poll_data[i+3].revents & POLLOUT) {
					int bytes = read(poll_data[i].fd, buff, 0xfff);
					write(poll_data[i+3].fd, buff, bytes);
				}
			}
		}
	}

	perror("box/poll");
	fprintf(stderr, "box: terminated\n");
	
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

void wrong_usage() {
	fprintf(stderr,
		"box: run program with stdI/O through FIFO\n"
		"  USAGE: box [-nname] executable [args...]\n"
		"	-n<name> change i/o files names. eg:\n"
		"		box -nfoo sh\n"
		"	  default is box. Extensions are added to each\n"
	);
	exit(1);
}

int main(int argc, char **argv){
	Signal_Handler(SIGINT, &cleanup);
	Signal_Handler(SIGTERM, &cleanup);
	Signal_Handler(SIGKILL, &cleanup);

	if(argc < 2) {
		wrong_usage();
	}

	uint i = 1;
	while(argv[i][0] == '-') {
		if(argv[i][1] == 'n') {
			char *name = argv[i]+2;
			sprintf(input_fifo, "%s.in", name);
			sprintf(output_fifo, "%s.out", name);
			sprintf(error_fifo, "%s.err", name);
			sprintf(ctl_fifo, "%s.ctl", name);
		} else {
			fprintf(stderr, "wrong argument: %s\n", argv[i]);
			wrong_usage();
		}
		i++;
	}

	char *args[argc - i + 1];
	uint j;
	for(j = 0; i+j < argc; j++) args[j] = argv[i+j];
	args[j] = NULL;

	//ctl_fifo mustn`t be a file!
	unlink(ctl_fifo);
	mkfifo(ctl_fifo, 0660);

	// Intentionally skip testing result so that I can have existing file to be
	// used as input, output or error.
	mkfifo(input_fifo, 0660);
	mkfifo(output_fifo, 0660);
	mkfifo(error_fifo, 0660);

	StreamSet program_streams = meanwhile(&execute, (void*)args);
	handle_streams(program_streams, (StreamFiles){
		.in = input_fifo, .out = output_fifo, .err = error_fifo, .ctl = ctl_fifo
	});

	return 0;
}