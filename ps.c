#include "types.h"
#include "stat.h"
#include "user.h"

int main()
{
    int a = fork();

    if (a == 0) //child
    {
        print_pinfo();
    }
    else //parent
    {
        wait();
    }

    exit();
}