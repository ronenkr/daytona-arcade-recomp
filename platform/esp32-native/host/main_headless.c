/* Headless scripted race on the host: runs nm_practice_run, which replays the NATIVE_RACE_SCRIPT input
   and prints the CHECKPOINT rows at the end. */
#include <stdio.h>
#include "practice.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: headless ASSET_DIR\n"); return 2; }
    return nm_practice_run(argv[1]);
}
