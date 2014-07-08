#ifndef __DASTRIE_PREFIX_H__
#define __DASTRIE_PREFIX_H__

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <set>
#include <algorithm>
#include <unordered_map>

#include "dastrie.h"
#include "util.h"

using namespace std;

int Init_Index(char *py_file, char *index_file);
int Deinit_Index();
int Get(string line, vector<string> &vRes);
int Reload_index(char *newindex_file);
int exiting();
#endif
