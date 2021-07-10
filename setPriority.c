#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
    int a = fork();
    if (a == 0) //child
    {
        set_priority(atoi(argv[2]), atoi(argv[1]));
    }
    else //parent
    {
        wait();
    }

    exit();
}