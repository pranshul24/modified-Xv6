//sample bench on which results are mentioned in report
#include "types.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"
#include "stat.h"

#define vll volatile long long int
#define MAX 100000000

int main(int argc, char *argv[])
{
	for (int i = 0; i < 10; i++)
	{
		int pid = fork();

		if (pid < 0)
		{
			printf(2, "Failed to fork\n");
			exit();
		}

		else if (pid == 0)
		{

			for (int j = 0; j < 10; j++)
			{
				if (j <= i)
					sleep(200);

				else
				{
					for (vll k = 0; k < MAX; k++)
						k = 1 ^ k; //cpu
				}
			}

			exit();
		}
	}
	for (int i = 0; i < 10; i++)
		wait();

	print_pinfo();

	exit();
}