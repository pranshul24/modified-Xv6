#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
	int a = fork();

	if (a == 0) //child
	{
		exec(argv[1], argv + 1);
	}
	else //parent
	{
		int *wtime = (int *)malloc(sizeof(int)),
			*rtime = (int *)malloc(sizeof(int));
		waitx(wtime, rtime);
		printf(1, "Runtime: %d, Wtime: %d\n", *rtime, *wtime);
	}

	exit();
}