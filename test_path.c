// cc -O0 -g -Wall test_path.c -o _test_path && ./_test_path
#define PATH_UNIT_TEST
#define PATH_IMPLEMENTATION
#include "path.h"
int main(int argc, char** argv)
{
    path_unit_test();
    return 0;
}
