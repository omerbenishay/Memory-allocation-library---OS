#include <unistd.h>
#include <iostream>
using namespace std;
void* smalloc(size_t size)
{
    if (size == 0 || size > 1e8 )
    {
        return nullptr;
    }
    void * result = sbrk(size);
    if (result == (void*)(-1))
    {
        return nullptr;
    }
    return result;
}