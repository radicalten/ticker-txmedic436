#ifndef OPTIONS_H
#define OPTIONS_H

#include <iostream>

const int QUOTE = 0x0001;

void PrintHelp();
void ProcessArgs(int *argc, char** argv[], int flag);

#endif
